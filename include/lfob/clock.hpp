#ifndef CLOCK_HPP_
#define CLOCK_HPP_

#include <chrono>
#include "types.hpp"

namespace lfob {

// Monotonic timestamp in nanoseconds. Use the same clock on the ingress path
// so (match_ts - ingress_ts) is meaningful
//
// steady_clock::now() lowers to clock_gettime(CLOCK_MONOTONIC) via the vDSO on
// Linux (~20ns, no syscall). If that shows up on the hot path, swap the body
// for a calibrated TSC read (__rdtsc) -- the call site does not change.
[[nodiscard]] inline Timestamp NowNanos() noexcept {
  return static_cast<Timestamp>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace lfob

#endif  // CLOCK_HPP_
