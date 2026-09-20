#ifndef REPORT_WIRE_HPP_
#define REPORT_WIRE_HPP_

#include <cstddef>
#include <cstdint>

#include "execution_report.hpp"

namespace lfob {

// <---- Report routing ---->

// Who is allowed to see a report
enum class Route : std::uint8_t {
  PRIVATE,  // owning client only
  PUBLIC,   // every subscribed client
  BOTH,     // e.g. a fill that also prints to the tape
};

[[nodiscard]] Route RouteOf(const ExecutionReport& report) noexcept;

// Wire encoding for a single report. Serializes `report` into `out` and
// returns the bytes written, or 0 if it does not fit in `max`. Shared on
// purpose: the output workers' normal fan-out and a session's own
// gap-notice/snapshot resync must frame reports identically, or the client
// stream would carry two layouts for one message and stop being parseable
[[nodiscard]] std::size_t EncodeReport(const ExecutionReport& report,
                                       std::byte* out,
                                       std::size_t max) noexcept;

}  // namespace lfob

#endif  // REPORT_WIRE_HPP_
