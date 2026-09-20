#include <lfob/report_wire.hpp>

#include <cstring>

namespace lfob {

Route RouteOf(const ExecutionReport& report) noexcept {
  switch (report.type) {
    case ExecutionReport::Type::FILL:
      return Route::BOTH;  // your ack + the public tape print
    case ExecutionReport::Type::TOP_OF_BOOK:
      return Route::PUBLIC;  // book state, everyone
    case ExecutionReport::Type::ACCEPTED:
    case ExecutionReport::Type::REJECTED:
    case ExecutionReport::Type::CANCELLED:
    case ExecutionReport::Type::REPLACED:
    case ExecutionReport::Type::GAP_NOTICE:
      return Route::PRIVATE;  // only the owning client
  }
  return Route::PRIVATE;  // unknown -> safest is owner-only
}

std::size_t EncodeReport(const ExecutionReport& report, std::byte* out,
                         std::size_t max) noexcept {
  // Current wire format is the raw report POD: fixed size, host order, one type
  // on the stream (see execution_report.hpp). Every producer routes through
  // here, so a future compact/portable encoding changes the format in exactly
  // one place and can never drift between the fan-out and resync paths
  if (sizeof(report) > max) {
    return 0;
  }
  std::memcpy(out, &report, sizeof(report));
  return sizeof(report);
}

}  // namespace lfob
