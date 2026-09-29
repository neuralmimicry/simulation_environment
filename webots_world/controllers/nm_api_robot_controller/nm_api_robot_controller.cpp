#include <webots/Robot.hpp>
#include <device_mapper.hpp>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
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
  std::string node_id;
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

std::string read_file(const std::string &path) {
  if (path.empty())
    return {};
  std::ifstream file(path);
  if (!file)
    return {};
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string env_or(const char *key, const char *fallback) {
  const char *value = std::getenv(key);
  return value && *value ? value : fallback;
}

size_t append_response(char *data, size_t size, size_t count, void *opaque) {
  const auto bytes = size * count;
  static_cast<std::string *>(opaque)->append(data, bytes);
  return bytes;
}

class InferenceWorker {
 public:
  InferenceWorker(std::string endpoint, std::string token, std::string network_id,
                  std::string node_id, json spike_io)
      : endpoint_(std::move(endpoint)),
        token_(std::move(token)),
        network_id_(std::move(network_id)),
        node_id_(std::move(node_id)),
        spike_io_(std::move(spike_io)),
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

  std::vector<std::uint32_t> outputs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outputs_;
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
  std::string endpoint_;
  std::string token_;
  std::string network_id_;
  std::string node_id_;
  json spike_io_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  Frame pending_;
  bool has_pending_ = false;
  bool stopping_ = false;
  std::vector<std::uint32_t> outputs_;
  std::thread thread_;

  bool infer(const Frame &frame, std::vector<std::uint32_t> &result) {
    CURL *curl = curl_easy_init();
    if (!curl)
      return false;

    json request = {{"network_id", network_id_},
                    {"step_index", frame.step},
                    {"time_ms", frame.time_ms},
                    {"dt_ms", frame.dt_ms},
                    {"aer_base", 0},
                    {"input_values", frame.values}};
    if (!node_id_.empty())
      request["node_id"] = node_id_;
    if (!spike_io_.is_null())
      request["spike_io"] = spike_io_;
    const std::string body = request.dump();
    std::string response;
    struct curl_slist *headers = nullptr;
    const std::string authorization = "Authorization: Bearer " + token_;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, authorization.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK || status < 200 || status >= 300) {
      std::cerr << "[nm_api_robot] infer request failed: "
                << (code == CURLE_OK ? "HTTP " + std::to_string(status) : curl_easy_strerror(code))
                << "\n";
      return false;
    }

    try {
      const auto parsed = json::parse(response);
      result = parsed.value("output_spike_indices", std::vector<std::uint32_t>{});
      return true;
    } catch (const std::exception &error) {
      std::cerr << "[nm_api_robot] invalid infer response: " << error.what() << "\n";
      return false;
    }
  }

  void run() {
    while (true) {
      Frame frame;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return stopping_ || has_pending_; });
        if (stopping_)
          return;
        frame = std::move(pending_);
        has_pending_ = false;
      }
      std::vector<std::uint32_t> next_outputs;
      if (infer(frame, next_outputs)) {
        std::lock_guard<std::mutex> lock(mutex_);
        outputs_ = std::move(next_outputs);
      }
    }
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
          entry.value("node_id", ""),
          std::regex(sensor_regex),
          std::regex(actuator_regex),
          entry.value("expected_sensory", std::size_t{0}),
          entry.value("expected_outputs", std::size_t{0})};
}
}  // namespace

int main() {
  Robot robot;
  const int timestep = static_cast<int>(robot.getBasicTimeStep());
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
    const std::string token_file = config.value("access_token_file", "");
    std::string token = read_file(token_file);
    token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char c) { return std::isspace(c); }), token.end());
    if (!binding.network_id.empty() && !token.empty()) {
      if ((binding.expected_sensory && sensor_indices.size() != binding.expected_sensory) ||
          (binding.expected_outputs && actuator_indices.size() != binding.expected_outputs)) {
        throw std::runtime_error("Webots device alignment differs from the configured AARNN profile");
      }
      curl_global_init(CURL_GLOBAL_DEFAULT);
      worker = std::make_unique<InferenceWorker>(api_base + "/aer/infer", token, binding.network_id,
                                                 binding.node_id, config.value("spike_io", json{}));
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
      std::max(20, config.value("inference_interval_ms", 200)));
  const auto output_hold = std::chrono::milliseconds(
      std::max(20, config.value("output_hold_ms", 350)));
  auto next_inference = std::chrono::steady_clock::now();
  std::vector<float> all_sensors(static_cast<std::size_t>(mapper.get_sensory_size()));
  std::vector<float> all_actuators(static_cast<std::size_t>(mapper.get_output_size()), 0.5f);
  std::vector<std::chrono::steady_clock::time_point> output_until(actuator_indices.size());
  std::uint64_t step_index = 0;

  while (robot.step(timestep) != -1) {
    ++step_index;
    if (worker) {
      mapper.fill_sensors(all_sensors);
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_inference) {
        Frame frame;
        frame.step = step_index;
        frame.time_ms = robot.getTime() * 1000.0;
        frame.dt_ms = timestep;
        frame.values.reserve(sensor_indices.size());
        for (int index : sensor_indices)
          frame.values.push_back(all_sensors[static_cast<std::size_t>(index)]);
        worker->submit(std::move(frame));
        next_inference = now + interval;
      }

      const auto outputs = worker->outputs();
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
