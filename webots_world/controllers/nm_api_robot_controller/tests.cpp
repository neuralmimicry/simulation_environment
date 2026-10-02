#include "coalesced_frame_metrics.hpp"
#include "inference_retry_policy.hpp"
#include "neural_effectors.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <vector>

int main() {
  using nm_webots::InjectionResult;

  assert(nm_webots::kSameFrameRetryWindow == std::chrono::seconds(120));
  assert(nm_webots::classify_injection_failure(true, 0) ==
         InjectionResult::retryable_failure);
  for (const long status : {408L, 425L, 429L, 500L, 503L, 599L})
    assert(nm_webots::classify_injection_failure(false, status) ==
           InjectionResult::retryable_failure);
  for (const long status : {0L, 400L, 401L, 403L, 404L, 422L, 600L})
    assert(nm_webots::classify_injection_failure(false, status) ==
           InjectionResult::permanent_failure);

  const long expected_delays[] = {250, 500, 1000, 2000, 4000, 8000, 16000, 16000};
  for (std::uint32_t index = 0; index < 8; ++index)
    assert(nm_webots::same_frame_retry_delay(index) ==
           std::chrono::milliseconds(expected_delays[index]));

  nm_webots::CoalescedFrameMetrics metrics;
  auto empty = metrics.take_snapshot();
  assert(empty.count == 0);
  assert(empty.total == 0);

  metrics.record_replacement(101);
  metrics.record_replacement(102);
  auto first_batch = metrics.take_snapshot();
  assert(first_batch.count == 2);
  assert(first_batch.first_step == 101);
  assert(first_batch.last_step == 102);
  assert(first_batch.total == 2);

  metrics.record_replacement(109);
  auto second_batch = metrics.take_snapshot();
  assert(second_batch.count == 1);
  assert(second_batch.first_step == 109);
  assert(second_batch.last_step == 109);
  assert(second_batch.total == 3);

  // The shared API bridge must turn admitted muscle-channel output into a
  // physical spine command; neutral channels must not generate movement.
  std::vector<std::string> worm_names;
  for (int segment = 1; segment <= 8; ++segment) {
    std::ostringstream suffix;
    suffix << std::setw(2) << std::setfill('0') << segment;
    worm_names.push_back("celegans_spine_" + suffix.str());
    for (const char *group : {"MDL", "MDR", "MVL", "MVR"})
      worm_names.push_back("celegans_o_001_" + std::string(group) + suffix.str());
  }
  nm_webots::CelegansSpineEffectors worm(32.0f);
  worm.discover(worm_names);
  assert(worm.active());
  std::vector<float> worm_commands(worm_names.size(), 0.5f);
  worm.apply(worm_commands);
  assert(std::fabs(worm_commands[0] - 0.5f) < 0.0001f);
  worm_commands[3] = 0.75f;  // MVL01
  worm_commands[4] = 0.75f;  // MVR01
  for (int step = 0; step < 6; ++step)
    worm.apply(worm_commands);
  assert(worm_commands[0] > 0.52f);

  std::vector<std::string> fly_names;
  for (int channel = 0; channel < 16; ++channel)
    fly_names.push_back("dros_o_" + std::to_string(channel) + "_motor");
  fly_names.push_back("wing_left_flap");
  fly_names.push_back("wing_right_flap");
  for (const char *side : {"left", "right"})
    for (const char *position : {"front", "mid", "rear"})
      for (const char *joint : {"coxa", "femur", "tibia", "tarsus"})
        fly_names.push_back("leg_" + std::string(side) + "_" + position + "_" + joint);
  nm_webots::FlyEffectors fly;
  fly.discover(fly_names);
  assert(fly.active());
  std::vector<float> fly_commands(fly_names.size(), 0.5f);
  assert(fly.apply(fly_commands, 8.0f) == 0.0f);
  for (int channel = 0; channel < 8; ++channel)
    fly_commands[channel] = 0.75f;
  const float fly_activity = fly.apply(fly_commands, 8.0f);
  assert(fly_activity > 0.0f && fly_activity <= 1.0f);
  assert(std::fabs(fly_commands[16] - 0.5f) > 0.001f);
  assert(std::any_of(fly_commands.begin() + 18, fly_commands.end(),
                     [](float value) { return std::fabs(value - 0.5f) > 0.001f; }));
  for (int channel = 0; channel < 8; ++channel)
    fly_commands[channel] = 0.5f;
  assert(fly.apply(fly_commands, 8.0f) == 0.0f);
}
