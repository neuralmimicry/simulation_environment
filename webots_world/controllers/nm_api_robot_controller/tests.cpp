#include "coalesced_frame_metrics.hpp"
#include "inference_retry_policy.hpp"

#include <cassert>
#include <chrono>

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
}
