#pragma once

#include <cstdint>

namespace nm_webots {

struct CoalescedFrameSnapshot {
  std::uint64_t count = 0;
  std::uint64_t first_step = 0;
  std::uint64_t last_step = 0;
  std::uint64_t total = 0;
};

// Guard this small accumulator with the worker's queue mutex.
class CoalescedFrameMetrics {
 public:
  void record_replacement(std::uint64_t replaced_step) {
    ++total_;
    if (since_report_ == 0)
      first_step_ = replaced_step;
    last_step_ = replaced_step;
    ++since_report_;
  }

  CoalescedFrameSnapshot take_snapshot() {
    CoalescedFrameSnapshot snapshot{since_report_, first_step_, last_step_, total_};
    since_report_ = 0;
    first_step_ = 0;
    last_step_ = 0;
    return snapshot;
  }

 private:
  std::uint64_t total_ = 0;
  std::uint64_t since_report_ = 0;
  std::uint64_t first_step_ = 0;
  std::uint64_t last_step_ = 0;
};

}  // namespace nm_webots
