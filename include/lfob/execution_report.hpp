#ifndef EXECUTION_REPORT_HPP_
#define EXECUTION_REPORT_HPP_

#include "types.hpp"

namespace lfob {

// The single POD the engine emits for everything the outside world needs
// to know. One type on one ring, so global sequencing is trivially
// correct: seq is strictly increasing with no gaps.
struct ExecutionReport {
  enum class Type : std::uint8_t {
    ACCEPTED,
    REJECTED,
    CANCELLED,
    REPLACED,
    FILL,       // one side of a trade
    TOP_OF_BOOK,  // BBO changed
  };

  enum class RejectReason : std::uint8_t {
    NONE,
    PRICE_OUT_OF_BAND,
    ZERO_QUANTITY,
    UNKNOWN_ORDER,
    ARENA_EXHAUSTED,
    INGRESS_FULL,
    FOK_UNFILLABLE,
  };

  SeqNum seq;
  Timestamp ingress_ts;  // stamped by the I/O thread
  Timestamp match_ts;    // stamped by the matching thread
  Type type;
  Side side;
  RejectReason reason;
  ClientId client;
  OrderId order_id;
  OrderId counterparty_id;  // Fill only
  Price price;
  Quantity qty;  // executed qty for Fill
  Quantity leaves_qty;
  Price bid_price;  // TopOfBook only
  Quantity bid_qty;
  Price ask_price;
  Quantity ask_qty;
};

static_assert(std::is_trivially_copyable_v<ExecutionReport>);

}  // namespace lfob

#endif // EXECUTION_REPORT_HPP_
