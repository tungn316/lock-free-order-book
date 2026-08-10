#ifndef ORDER_HPP_
#define ORDER_HPP_
#include "types.hpp"

namespace lfob {

// A resting order node living inside NodeArena's flat vector.
// Links are NodeIdx, not pointers: 32-bit, relocation-safe, and the
// whole node fits comfortably inside one cache line.
struct Order {
  OrderId id;
  Price price;
  Quantity remaining;
  NodeIdx prev;  // FIFO neighbours within the price level
  NodeIdx next;
  std::uint32_t level_idx;  // owning level's tick index, for O(1) cancel
  ClientId client;
  Side side;
  TimeInForce tif;
};

static_assert(std::is_trivially_copyable_v<Order>);

}  // namespace lfob

#endif // ORDER_HPP_
