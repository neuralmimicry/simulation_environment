#include <webots/Robot.hpp>
#include <device_mapper.hpp>

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
                        const std::string *post_body = nullptr) {
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
  // The admission path can wait for the previous bounded sensory slot to be
  // consumed by a busy executor. Simulation stepping remains independent on
  // this worker thread, and the pending-frame queue stays size one.
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 25000L);
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
      pending_ = std::move(frame);  // A slow API call drops stale frames, never grows a queue.
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
  std::vector<std::uint32_t> outputs_;
  bool outputs_pending_ = false;
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
                                        const std::string *post_body = nullptr) {
    HttpResult response = perform_http(curl, url, token_, post_body);
    if (response.code != CURLE_OK || response.status != 401 || !reload_rotated_token())
      return response;

    HttpResult retry = perform_http(curl, url, token_, post_body);
    retry.total_time_seconds += response.total_time_seconds;
    retry.new_connections += response.new_connections;
    return retry;
  }

  bool infer(CURL *curl, const Frame &frame, std::vector<std::uint32_t> &result,
             double &request_ms, long &new_connections) {
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

    const HttpResult injection =
        perform_authenticated_http(curl, api_base_ + "/aer/inject", &body);
    account_request(injection);
    if (injection.code != CURLE_OK || injection.status < 200 || injection.status >= 300) {
      log_http_failure("inject", injection);
      return false;
    }

    char *escaped_network = curl_easy_escape(
        curl, network_id_.c_str(), static_cast<int>(network_id_.size()));
    if (!escaped_network) {
      std::cerr << "[nm_api_robot] cannot encode activity query robot=" << robot_name_
                << " network=" << network_id_ << std::endl;
      return false;
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
        return false;
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
      log_http_failure("activity", activity);
      return false;
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
      return true;
    } catch (const std::exception &error) {
      std::cerr << "[nm_api_robot] invalid activity response robot=" << robot_name_
                << " network=" << network_id_ << " step=" << frame.step
                << " reason=" << error.what() << "\n";
      return false;
    }
  }

  void run() {
    CURL *curl = curl_easy_init();
    if (!curl) {
      std::cerr << "[nm_api_robot] cannot create HTTP client robot=" << robot_name_
                << " network=" << network_id_ << std::endl;
      return;
    }

    std::uint64_t successful_frames = 0;
    std::uint32_t consecutive_failures = 0;
    auto retry_after = std::chrono::steady_clock::time_point::min();
    auto next_status_log = std::chrono::steady_clock::now();
    while (true) {
      Frame frame;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return stopping_ || has_pending_; });
        if (retry_after > std::chrono::steady_clock::now())
          condition_.wait_until(lock, retry_after, [this] { return stopping_; });
        if (stopping_)
          break;
        frame = std::move(pending_);
        has_pending_ = false;
      }
      std::vector<std::uint32_t> next_outputs;
      double request_ms = 0.0;
      long new_connections = 0;
      if (infer(curl, frame, next_outputs, request_ms, new_connections)) {
        const auto mapped_outputs = static_cast<std::size_t>(std::count_if(
            next_outputs.begin(), next_outputs.end(),
            [this](std::uint32_t index) { return index < actuator_count_; }));
        {
          std::lock_guard<std::mutex> lock(mutex_);
          outputs_ = next_outputs;
          outputs_pending_ = true;
        }
        ++successful_frames;
        const auto now = std::chrono::steady_clock::now();
        if (successful_frames == 1 || now >= next_status_log) {
          std::cout << "[nm_api_robot] inference ok robot=" << robot_name_
                    << " network=" << network_id_
                    << " successful_frames=" << successful_frames
                    << " step=" << frame.step
                    << " input_spikes=" << std::count_if(
                           frame.values.begin(), frame.values.end(),
                           [this](float value) {
                             return std::isfinite(value) && value >= input_spike_threshold_;
                           })
                    << " output_spikes=" << next_outputs.size()
                    << " mapped_actuators=" << mapped_outputs
                    << " request_ms=" << static_cast<long>(request_ms)
                    << " new_connections=" << new_connections << std::endl;
          next_status_log = now + std::chrono::seconds(30);
        }
        consecutive_failures = 0;
        retry_after = std::chrono::steady_clock::time_point::min();
      } else {
        ++consecutive_failures;
        const auto exponent = std::min<std::uint32_t>(consecutive_failures - 1, 6);
        const auto delay = std::min(std::chrono::milliseconds(30000),
                                    std::chrono::milliseconds(500 * (1u << exponent)));
        retry_after = std::chrono::steady_clock::now() + delay;
        std::cerr << "[nm_api_robot] retry scheduled robot=" << robot_name_
                  << " network=" << network_id_ << " delay_ms=" << delay.count()
                  << " consecutive_failures=" << consecutive_failures << std::endl;
      }
    }
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
      for (std::size_t i = 0; i < actuator_indices.size(); ++i)
        if (output_until[i] > now)
          all_actuators[static_cast<std::size_t>(actuator_indices[i])] = 0.75f;
      mapper.apply_actuators(all_actuators);
    }
  }

  if (worker)
    worker->stop();
  curl_global_cleanup();
  return 0;
}
