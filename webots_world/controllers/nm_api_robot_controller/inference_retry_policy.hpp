#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace nm_webots {

inline constexpr std::chrono::seconds kSameFrameRetryWindow{120};

enum class InjectionResult {
  admitted,
  retryable_failure,
  permanent_failure,
};

inline InjectionResult classify_injection_failure(bool transport_failure,
                                                   long http_status) {
  if (transport_failure || http_status == 408 || http_status == 425 ||
      http_status == 429 || (http_status >= 500 && http_status <= 599))
    return InjectionResult::retryable_failure;
  return InjectionResult::permanent_failure;
}

inline bool is_retryable_injection_failure(bool transport_failure, long http_status) {
  return classify_injection_failure(transport_failure, http_status) ==
         InjectionResult::retryable_failure;
}

inline std::chrono::milliseconds same_frame_retry_delay(std::uint32_t retry_index) {
  constexpr std::array<long, 7> delays_ms{250, 500, 1000, 2000, 4000, 8000, 16000};
  const auto index = std::min<std::size_t>(retry_index, delays_ms.size() - 1);
  return std::chrono::milliseconds(delays_ms[index]);
}

}  // namespace nm_webots
