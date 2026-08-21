#ifndef TYPES_HPP_
#define TYPES_HPP_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lfob {

using Price = std::uint64_t;
using Quantity = std::uint64_t;
using OrderId = std::uint64_t;
using SeqNum = std::uint64_t;
using ClientId = std::uint32_t;
using Timestamp = std::uint64_t;
using NodeIdx = std::uint32_t;
using Generation = std::uint32_t;

enum class Side : std::uint8_t { BID, ASK };
enum class TimeInForce : std::uint8_t { DAY, IOC, FOK };

// Used nin 
inline constexpr NodeIdx k_null_node{0xFFFF'FFFFU};

// Cache line size used to pad atomics
inline constexpr std::size_t k_cache_line{64};

// Used in book_side for empty side with no resting orders so no 'best' order
inline constexpr std::uint32_t k_no_best{0xFFFF'FFFFU};

inline constexpr Price k_no_price{0xFFFF'FFFF'FFFF'FFFFULL};

// During setup of consumer threads
inline constexpr std::uint32_t k_invalid_consumer{0xFFFF'FFFFU};

// Matching Engine batch size for TryPushBulk
inline constexpr std::size_t k_stage_batch{64};

// Stable handle to node, generation guards against recycled slot being
// addressed by a stale OrderID
struct NodeRef {
  NodeIdx idx;
  Generation gen;
};

// The single canonical unit of work crossing the ingress boundary.
// Built by an I/O thread after parse + validation, consumed only by the
// matching thread. Trivially copyable so it can live in a ring slot.
struct OrderCommand {
  enum class Type : std::uint8_t { NEW, CANCEL, REPLACE };

  Type type;
  Side side;
  TimeInForce tif;
  ClientId client;       // who to send the ack/fill back to
  OrderId id;            // target order (New: newly assigned id)
  Price price;           // ticks; ignored for Cancel
  Quantity quantity;     // ignored for Cancel
  Timestamp ingress_ts;  // stamped by the I/O thread for latency accounting
};

// Seqlock holds Bbo Snapshots
struct Bbo {
  SeqNum event_seq;
  Price bid_price;
  Quantity bid_quantity;
  Price ask_price;
  Quantity ask_quantity;
};

static_assert(std::is_trivially_copyable_v<OrderCommand>);

}  // namespace lfob

#endif  // TYPES_HPP_
