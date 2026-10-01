#include <webots/Robot.hpp>
#include <device_mapper.hpp>
#include "coalesced_frame_metrics.hpp"
#include "inference_retry_policy.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using nlohmann::json;
using webots::DeviceMapper;
using webots::Robot;

namespace {
struct Binding {
  std::string network_id;
  std::regex sensor_pattern;
  std::regex actuator_pattern;
  std::size_t expected_sensory = 0;
  std::size_t expected_outputs = 0;
};

struct Frame {
  std::uint64_t step = 0;
  double time_ms = 0.0;
  double dt_ms = 0.0;
  std::vector<float> values;
};

struct HttpResult {
  CURLcode code = CURLE_OK;
  long status = 0;
  double total_time_seconds = 0.0;
  long new_connections = 0;
  std::string body;
};

std::string read_file(const std::string &path) {
  if (path.empty())
    return {};
  std::ifstream file(path);
  if (!file)
    return {};
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string read_token(const std::string &path) {
  std::string token = read_file(path);
  token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char ch) {
                return std::isspace(ch);
              }),
              token.end());
  return token;
}

double load_world_elapsed_seconds(const std::string &world_path) {
  if (world_path.empty())
    return 0.0;
  std::ifstream input(world_path + ".clock");
  double elapsed = 0.0;
  if (input >> elapsed && std::isfinite(elapsed) && elapsed > 0.0)
    return elapsed;
  return 0.0;
}

std::string env_or(const char *key, const char *fallback) {
  const char *value = std::getenv(key);
  return value && *value ? value : fallback;
}

std::string make_session_id(const std::string &robot_name) {
  std::string safe_name;
  safe_name.reserve(std::min<std::size_t>(robot_name.size(), 48));
  for (const unsigned char ch : robot_name) {
    if (safe_name.size() == 48)
      break;
    safe_name.push_back(std::isalnum(ch) || ch == '-' || ch == '_' ? ch : '-');
  }
  if (safe_name.empty())
    safe_name = "robot";
  const auto epoch = std::chrono::steady_clock::now().time_since_epoch().count();
  return "webots-" + safe_name + "-" + std::to_string(epoch);
}

size_t append_response(char *data, size_t size, size_t count, void *opaque) {
  const auto bytes = size * count;
  static_cast<std::string *>(opaque)->append(data, bytes);
  return bytes;
}

HttpResult perform_http(CURL *curl, const std::string &url,
                        const std::string &token,
                        const std::string *post_body = nullptr,
                        long timeout_ms = 25000L) {
  curl_easy_reset(curl);
  HttpResult result;
  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");
  if (post_body)
    headers = curl_slist_append(headers, "Content-Type: application/json");
  const std::string authorization = "Authorization: Bearer " + token;
  headers = curl_slist_append(headers, authorization.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  // Bound each request by the remaining same-frame retry budget. Simulation
  // stepping continues on its independent controller thread.
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, std::max(1L, timeout_ms));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (post_body) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body->c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(post_body->size()));
  } else {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  }

  result.code = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
  curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &result.total_time_seconds);
  curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &result.new_connections);
  // Reset borrowed request pointers but preserve libcurl's live connection cache.
  curl_easy_reset(curl);
  curl_slist_free_all(headers);
  return result;
}

class InferenceWorker {
 public:
  InferenceWorker(std::string api_base, std::string activity_addr,
                  std::string token_file, std::string token,
                  std::string network_id, std::string robot_name,
                  std::size_t actuator_count, float input_spike_threshold)
      : api_base_(std::move(api_base)),
        activity_addr_(std::move(activity_addr)),
        token_file_(std::move(token_file)),
        token_(std::move(token)),
        network_id_(std::move(network_id)),
        robot_name_(std::move(robot_name)),
        session_id_(make_session_id(robot_name_)),
        actuator_count_(actuator_count),
        input_spike_threshold_(std::isfinite(input_spike_threshold)
                                   ? std::clamp(input_spike_threshold, 0.0f, 1.0f)
                                   : 0.5f),
        thread_([this] { run(); }) {}

  ~InferenceWorker() { stop(); }

  void submit(Frame frame) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_ || session_stopped_)
        return;
      if (has_pending_)
        coalesced_frames_.record_replacement(pending_.step);
      pending_ = std::move(frame);  // Keep only the latest unadmitted sample.
      has_pending_ = true;
    }
    condition_.notify_one();
  }

  bool take_outputs(std::vector<std::uint32_t> &outputs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!outputs_pending_)
      return false;
    outputs = std::move(outputs_);
    outputs_pending_ = false;
    return true;
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_one();
    if (thread_.joinable())
      thread_.join();
  }

 private:
  std::string api_base_;
  std::string activity_addr_;
  std::string token_file_;
  std::string token_;
  std::string network_id_;
  std::string robot_name_;
  std::string session_id_;
  std::size_t actuator_count_;
  float input_spike_threshold_;
  std::uint64_t last_activity_step_ = 0;
  bool has_last_activity_step_ = false;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  Frame pending_;
  bool has_pending_ = false;
  bool stopping_ = false;
  bool session_stopped_ = false;
  std::vector<std::uint32_t> outputs_;
  bool outputs_pending_ = false;
  nm_webots::CoalescedFrameMetrics coalesced_frames_;
  std::chrono::steady_clock::time_point next_activity_warning_ =
      std::chrono::steady_clock::time_point::min();
  std::thread thread_;

  bool reload_rotated_token() {
    const std::string current = read_token(token_file_);
    if (current.empty() || current == token_)
      return false;
    token_ = current;
    std::cout << "[nm_api_robot] reloaded rotated service credential robot="
              << robot_name_ << " network=" << network_id_ << std::endl;
    return true;
  }

  HttpResult perform_authenticated_http(CURL *curl, const std::string &url,
                                        const std::string *post_body = nullptr,
                                        long timeout_ms = 25000L) {
    const auto started = std::chrono::steady_clock::now();
    HttpResult response = perform_http(curl, url, token_, post_body, timeout_ms);
    if (response.code != CURLE_OK || response.status != 401 || !reload_rotated_token())
      return response;

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    const long remaining_timeout_ms =
        std::max(1L, timeout_ms - static_cast<long>(elapsed_ms));
    HttpResult retry = perform_http(curl, url, token_, post_body, remaining_timeout_ms);
    retry.total_time_seconds += response.total_time_seconds;
    retry.new_connections += response.new_connections;
    return retry;
  }

  nm_webots::InjectionResult infer(
      CURL *curl, const Frame &frame, std::vector<std::uint32_t> &result,
      bool &activity_available, double &request_ms, long &new_connections,
      long injection_timeout_ms) {
    // Leave the injection address unset so the orchestrator can choose the
    // current sensory bridge and preserve cluster fan-out. A configured worker
    // address may be a backup or become stale after a placement change.
    std::vector<std::uint32_t> input_spike_indices;
    input_spike_indices.reserve(frame.values.size());
    for (std::size_t i = 0; i < frame.values.size(); ++i) {
      const float value = frame.values[i];
      if (std::isfinite(value) && value >= input_spike_threshold_)
        input_spike_indices.push_back(static_cast<std::uint32_t>(i));
    }

    json request = {{"network_id", network_id_},
                    {"session_id", session_id_},
                    {"step_index", frame.step},
                    {"time_ms", frame.time_ms},
                    {"dt_ms", frame.dt_ms},
                    {"aer_base", 0},
                    {"spike_indices", input_spike_indices}};
    const std::string body = request.dump();
    request_ms = 0.0;
    new_connections = 0;
    activity_available = false;
    auto account_request = [&](const HttpResult &http) {
      request_ms += http.total_time_seconds * 1000.0;
      new_connections += http.new_connections;
    };
    auto log_http_failure = [&](const char *stage, const HttpResult &http) {
      constexpr std::size_t max_error_body = 512;
      std::string response = http.body;
      const bool body_truncated = response.size() > max_error_body;
      if (body_truncated)
        response.resize(max_error_body);
      std::cerr << "[nm_api_robot] " << stage << " request failed robot=" << robot_name_
                << " network=" << network_id_ << " step=" << frame.step << " reason="
                << (http.code == CURLE_OK ? "HTTP " + std::to_string(http.status)
                                          : curl_easy_strerror(http.code))
                << " request_ms=" << static_cast<long>(request_ms)
                << " new_connections=" << new_connections;
      if (!response.empty())
        std::cerr << " response=" << response << (body_truncated ? "...[truncated]" : "");
      std::cerr << std::endl;
    };

    const HttpResult injection = perform_authenticated_http(
        curl, api_base_ + "/simulation/aer/inject", &body, injection_timeout_ms);
    account_request(injection);
    if (injection.code != CURLE_OK || injection.status < 200 || injection.status >= 300) {
      log_http_failure("inject", injection);
      return nm_webots::classify_injection_failure(injection.code != CURLE_OK,
                                                   injection.status);
    }

    char *escaped_network = curl_easy_escape(
        curl, network_id_.c_str(), static_cast<int>(network_id_.size()));
    if (!escaped_network) {
      std::cerr << "[nm_api_robot] cannot encode activity query robot=" << robot_name_
                << " network=" << network_id_ << std::endl;
      return nm_webots::InjectionResult::admitted;
    }
    const std::string network_query = escaped_network;
    curl_free(escaped_network);
    std::string activity_url = api_base_ + "/activity?network_id=" + network_query;
    if (!activity_addr_.empty()) {
      char *escaped_addr = curl_easy_escape(
          curl, activity_addr_.c_str(), static_cast<int>(activity_addr_.size()));
      if (!escaped_addr) {
        std::cerr << "[nm_api_robot] cannot encode activity address robot=" << robot_name_
                  << " network=" << network_id_ << std::endl;
        return nm_webots::InjectionResult::admitted;
      }
      activity_url += "&addr=";
      activity_url += escaped_addr;
      curl_free(escaped_addr);
    }

    HttpResult activity = perform_authenticated_http(curl, activity_url);
    account_request(activity);
    if ((activity.code != CURLE_OK || activity.status >= 500) && !activity_addr_.empty()) {
      // The cached direct node may be offline. Let the API's normal cluster
      // discovery select a replacement, then follow its returned source.
      const std::string fallback_url = api_base_ + "/activity?network_id=" + network_query;
      activity = perform_authenticated_http(curl, fallback_url);
      account_request(activity);
    }
    if (activity.code != CURLE_OK || activity.status < 200 || activity.status >= 300) {
      // Sensory admission already succeeded. The AARNN activity endpoint is
      // a best-effort projection and may return 503 while the network is
      // traversing; do not treat that as a rejected frame or back off input.
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_activity_warning_) {
        log_http_failure("activity projection", activity);
        next_activity_warning_ = now + std::chrono::seconds(30);
      }
      return nm_webots::InjectionResult::admitted;
    }

    try {
      const auto parsed = json::parse(activity.body);
      const std::uint64_t sim_step = parsed.value("sim_step", std::uint64_t{0});
      const bool step_reset = has_last_activity_step_ && sim_step < last_activity_step_;
      const std::uint64_t previous_step = step_reset ? 0 : last_activity_step_;
      if (!has_last_activity_step_ || step_reset || sim_step > previous_step) {
        const auto output = parsed.value("output", json::object());
        if (output.is_object())
          result = output.value("indices", std::vector<std::uint32_t>{});
        if (result.empty()) {
          const auto history = parsed.value("output_history", json::array());
          if (history.is_array()) {
            for (const auto &entry : history) {
              if (!entry.is_object() || entry.value("step", std::uint64_t{0}) <= previous_step)
                continue;
              const auto indices = entry.value("indices", std::vector<std::uint32_t>{});
              if (!indices.empty()) {
                result = indices;
                break;
              }
            }
          }
        }
      }
      last_activity_step_ = sim_step;
      has_last_activity_step_ = true;
      activity_available = true;
      return nm_webots::InjectionResult::admitted;
    } catch (const std::exception &error) {
      std::cerr << "[nm_api_robot] invalid activity response robot=" << robot_name_
                << " network=" << network_id_ << " step=" << frame.step
                << " reason=" << error.what() << "\n";
      // The sensory frame was admitted; a malformed optional projection must
      // not trigger exponential backoff for later controller frames.
      return nm_webots::InjectionResult::admitted;
    }
  }

  void stop_input_session(const char *reason, const Frame &frame,
                          std::chrono::steady_clock::duration elapsed) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      session_stopped_ = true;
      has_pending_ = false;
    }
    condition_.notify_one();
    std::cerr << "[nm_api_robot] input session stopped robot=" << robot_name_
              << " network=" << network_id_ << " session=" << session_id_
              << " step=" << frame.step << " reason=" << reason
              << " elapsed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
              << std::endl;
  }

  void expire_input_frame(const Frame &frame,
                          std::chrono::steady_clock::duration elapsed) const {
    // The old frame cannot be safely retried forever, but a transient
    // orchestrator/bridge outage must not permanently disable this robot.
    // submit() keeps only the newest world frame, so the worker can resume
    // from the shared world clock as soon as the route becomes available.
    std::cerr << "[nm_api_robot] sensory frame expired robot=" << robot_name_
              << " network=" << network_id_ << " session=" << session_id_
              << " step=" << frame.step << " reason=same-frame retry window expired"
              << " elapsed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
              << " action=resume with latest world frame" << std::endl;
  }

  void log_coalesced_frames(const nm_webots::CoalescedFrameSnapshot &snapshot) const {
    if (snapshot.count == 0)
      return;
    std::clog << "[nm_api_robot] unadmitted frames coalesced robot=" << robot_name_
              << " network=" << network_id_
              << " count_since_report=" << snapshot.count
              << " first_skipped_world_step=" << snapshot.first_step
              << " last_skipped_world_step=" << snapshot.last_step
              << " total_coalesced=" << snapshot.total << std::endl;
  }

  void run() {
    CURL *curl = curl_easy_init();
    if (!curl) {
      std::cerr << "[nm_api_robot] cannot create HTTP client robot=" << robot_name_
                << " network=" << network_id_ << std::endl;
      std::lock_guard<std::mutex> lock(mutex_);
      session_stopped_ = true;
      return;
    }

    std::uint64_t successful_frames = 0;
    auto next_status_log = std::chrono::steady_clock::now();
    while (true) {
      Frame frame;
      nm_webots::CoalescedFrameSnapshot coalesced_snapshot;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] {
          return stopping_ || session_stopped_ || has_pending_;
        });
        if (stopping_ || session_stopped_)
          break;
        frame = std::move(pending_);
        has_pending_ = false;
        coalesced_snapshot = coalesced_frames_.take_snapshot();
      }
      log_coalesced_frames(coalesced_snapshot);

      std::vector<std::uint32_t> next_outputs;
      bool activity_available = false;
      double total_request_ms = 0.0;
      long total_new_connections = 0;
      const auto retry_started = std::chrono::steady_clock::now();
      std::uint32_t retry_index = 0;
      bool frame_finished = false;
      while (!frame_finished) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (stopping_) {
            frame_finished = true;
            break;
          }
        }

        const auto before_request = std::chrono::steady_clock::now();
        const auto elapsed = before_request - retry_started;
        const auto remaining = nm_webots::kSameFrameRetryWindow - elapsed;
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
          expire_input_frame(frame, elapsed);
          frame_finished = true;
          break;
        }
        const long timeout_ms = std::max(
            1L, static_cast<long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
                        .count()));

        double attempt_request_ms = 0.0;
        long attempt_new_connections = 0;
        const auto injection_result = infer(
            curl, frame, next_outputs, activity_available, attempt_request_ms,
            attempt_new_connections, timeout_ms);
        total_request_ms += attempt_request_ms;
        total_new_connections += attempt_new_connections;

        if (injection_result == nm_webots::InjectionResult::admitted) {
          const auto mapped_outputs = static_cast<std::size_t>(std::count_if(
              next_outputs.begin(), next_outputs.end(),
              [this](std::uint32_t index) { return index < actuator_count_; }));
          if (activity_available) {
            std::lock_guard<std::mutex> lock(mutex_);
            outputs_ = next_outputs;
            outputs_pending_ = true;
          }
          ++successful_frames;
          const auto now = std::chrono::steady_clock::now();
          if (successful_frames == 1 || now >= next_status_log) {
            std::cout << "[nm_api_robot] sensory frame admitted robot="
                      << robot_name_ << " network=" << network_id_
                      << " admitted_frames=" << successful_frames
                      << " step=" << frame.step << " input_spikes="
                      << std::count_if(frame.values.begin(), frame.values.end(),
                                       [this](float value) {
                                         return std::isfinite(value) &&
                                                value >= input_spike_threshold_;
                                       })
                      << " activity_snapshot="
                      << (activity_available ? "available" : "pending");
            if (activity_available)
              std::cout << " output_spikes=" << next_outputs.size()
                        << " mapped_actuators=" << mapped_outputs;
            std::cout << " request_ms=" << static_cast<long>(total_request_ms)
                      << " new_connections=" << total_new_connections << std::endl;
            next_status_log = now + std::chrono::seconds(30);
          }
          frame_finished = true;
        } else if (injection_result == nm_webots::InjectionResult::permanent_failure) {
          stop_input_session("permanent injection error", frame,
                             std::chrono::steady_clock::now() - retry_started);
          frame_finished = true;
        } else {
          const auto retry_now = std::chrono::steady_clock::now();
          const auto retry_elapsed = retry_now - retry_started;
          const auto retry_remaining = nm_webots::kSameFrameRetryWindow - retry_elapsed;
          if (retry_remaining <= std::chrono::steady_clock::duration::zero()) {
            expire_input_frame(frame, retry_elapsed);
            frame_finished = true;
            break;
          }
          const auto policy_delay = nm_webots::same_frame_retry_delay(retry_index++);
          const auto delay = std::min(
              policy_delay,
              std::chrono::duration_cast<std::chrono::milliseconds>(retry_remaining));
          std::cerr << "[nm_api_robot] retrying same sensory frame robot="
                    << robot_name_ << " network=" << network_id_
                    << " session=" << session_id_ << " step=" << frame.step
                    << " delay_ms=" << delay.count()
                    << " elapsed_ms="
                    << std::chrono::duration_cast<std::chrono::milliseconds>(retry_elapsed).count()
                    << " retry_index=" << retry_index << std::endl;
          std::unique_lock<std::mutex> lock(mutex_);
          condition_.wait_for(lock, delay, [this] { return stopping_; });
          if (stopping_) {
            frame_finished = true;
            break;
          }
        }
      }
    }

    nm_webots::CoalescedFrameSnapshot final_coalesced_snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      final_coalesced_snapshot = coalesced_frames_.take_snapshot();
    }
    log_coalesced_frames(final_coalesced_snapshot);
    curl_easy_cleanup(curl);
  }
};

Binding load_binding(const json &config, const std::string &robot_name) {
  const auto robots = config.value("robots", json::object());
  const auto found = robots.find(robot_name);
  if (found == robots.end())
    throw std::runtime_error("no fleet binding for robot " + robot_name);
  const auto &entry = *found;
  const std::string sensor_regex = entry.value("sensor_regex", "^celegans_s_.*$");
  const std::string actuator_regex = entry.value("actuator_regex", "^celegans_o_.*$");
  return {entry.value("network_id", ""),
          std::regex(sensor_regex),
          std::regex(actuator_regex),
          entry.value("expected_sensory", std::size_t{0}),
          entry.value("expected_outputs", std::size_t{0})};
}
}  // namespace

int main() {
  Robot robot;
  const int timestep = static_cast<int>(robot.getBasicTimeStep());
  const double world_elapsed_before_start = load_world_elapsed_seconds(
      env_or("NM_WEBOTS_RUNTIME_WORLD_FILE", ""));
  DeviceMapper mapper;
  mapper.discover(robot, timestep);

  const std::string robot_name = robot.getName();
  const std::string config_path = env_or("NM_WEBOTS_FLEET_CONFIG", "/etc/neuralmimicry/webots/fleet.json");
  json config = json::object();
  const std::string config_contents = read_file(config_path);
  try {
    if (!config_contents.empty()) {
      config = json::parse(config_contents);
      if (!config.is_object())
        throw std::runtime_error("fleet config root must be a JSON object");
    } else {
      std::cerr << "[nm_api_robot] fleet config is unavailable at " << config_path
                << "; robot will remain safely unbound\n";
    }
  } catch (const std::exception &error) {
    std::cerr << "[nm_api_robot] cannot load fleet config " << config_path << ": " << error.what() << "\n";
    config = json::object();
  }

  std::unique_ptr<InferenceWorker> worker;
  Binding binding;
  std::vector<int> sensor_indices;
  std::vector<int> actuator_indices;
  const auto sensor_names = mapper.get_sensor_names();
  const auto actuator_names = mapper.get_actuator_names();
  try {
    binding = load_binding(config, robot_name);
    for (std::size_t i = 0; i < sensor_names.size(); ++i)
      if (std::regex_match(sensor_names[i], binding.sensor_pattern))
        sensor_indices.push_back(static_cast<int>(i));
    for (std::size_t i = 0; i < actuator_names.size(); ++i)
      if (std::regex_match(actuator_names[i], binding.actuator_pattern))
        actuator_indices.push_back(static_cast<int>(i));

    const std::string api_base = config.value("api_base", "https://aarnn.neuralmimicry.ai/api");
    const std::string activity_addr = config.value("activity_addr", "");
    const std::string token_file = config.value("access_token_file", "");
    std::string token = read_token(token_file);
    if (!binding.network_id.empty() && !token.empty()) {
      if ((binding.expected_sensory && sensor_indices.size() != binding.expected_sensory) ||
          (binding.expected_outputs && actuator_indices.size() != binding.expected_outputs)) {
        throw std::runtime_error("Webots device alignment differs from the configured AARNN profile");
      }
      curl_global_init(CURL_GLOBAL_DEFAULT);
      worker = std::make_unique<InferenceWorker>(api_base, activity_addr,
                                                 token_file, token,
                                                 binding.network_id, robot_name,
                                                 actuator_indices.size(),
                                                 config.value("input_spike_threshold", 0.5f));
      std::cout << "[nm_api_robot] connected robot=" << robot_name << " network=" << binding.network_id
                << " sensors=" << sensor_indices.size() << " actuators=" << actuator_indices.size() << "\n";
    } else {
      std::cout << "[nm_api_robot] robot=" << robot_name
                << " has no active AARNN binding; set its network_id and access_token_file in "
                << config_path << "\n";
    }
  } catch (const std::exception &error) {
    std::cerr << "[nm_api_robot] binding disabled for " << robot_name << ": " << error.what() << "\n";
  }

  const auto interval = std::chrono::milliseconds(
      std::max(20, config.value("inference_interval_ms", 1000)));
  const auto output_hold = std::chrono::milliseconds(
      std::max(20, config.value("output_hold_ms", 1500)));
  auto next_inference = std::chrono::steady_clock::now();
  std::vector<float> all_sensors(static_cast<std::size_t>(mapper.get_sensory_size()));
  std::vector<float> all_actuators(static_cast<std::size_t>(mapper.get_output_size()), 0.5f);
  std::vector<std::chrono::steady_clock::time_point> output_until(actuator_indices.size());
  auto next_motor_status_log = std::chrono::steady_clock::time_point::min();

  while (robot.step(timestep) != -1) {
    if (worker) {
      mapper.fill_sensors(all_sensors);
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_inference) {
        const double world_time_ms =
            (world_elapsed_before_start + robot.getTime()) * 1000.0;
        const auto world_step = std::llround(world_time_ms / static_cast<double>(timestep));
        Frame frame;
        // Sparse AARNN ingress sequences frames by step_index. Derive it from
        // the shared, persisted world clock instead of advancing a private
        // controller counter, so every network sees the same world timeline.
        frame.step = world_step > 0 ? static_cast<std::uint64_t>(world_step) : 0;
        // Webots supplies one simulation clock to every robot and the ecology
        // supervisor. Restore the persisted epoch so a simulator restart does
        // not move sensory timestamps backwards relative to the living network.
        frame.time_ms = world_time_ms;
        frame.dt_ms = timestep;
        frame.values.reserve(sensor_indices.size());
        for (int index : sensor_indices)
          frame.values.push_back(all_sensors[static_cast<std::size_t>(index)]);
        worker->submit(std::move(frame));
        next_inference = now + interval;
      }

      std::vector<std::uint32_t> outputs;
      if (worker->take_outputs(outputs))
        for (std::uint32_t index : outputs)
          if (index < output_until.size())
            output_until[index] = now + output_hold;
      std::fill(all_actuators.begin(), all_actuators.end(), 0.5f);
      std::size_t active_motors = 0;
      for (std::size_t i = 0; i < actuator_indices.size(); ++i)
        if (output_until[i] > now) {
          all_actuators[static_cast<std::size_t>(actuator_indices[i])] = 0.75f;
          ++active_motors;
        }
      mapper.apply_actuators(all_actuators);
      if (active_motors > 0 && now >= next_motor_status_log) {
        std::cout << "[nm_api_robot] motor outputs applied robot=" << robot_name
                  << " network=" << binding.network_id
                  << " active_motors=" << active_motors << std::endl;
        next_motor_status_log = now + std::chrono::seconds(30);
      }
    }
  }

  if (worker)
    worker->stop();
  curl_global_cleanup();
  return 0;
}
