#ifndef PRICE_LEVEL_HPP_
#define PRICE_LEVEL_HPP_

#include <cstddef>
#include "node_arena.hpp"
#include "types.hpp"

namespace lfob {

// Orders resting at one price in time priority. Holds only indices;
// the arena is passed in so the level itself stays 40-ish bytes and
// packs densely inside BookSide's dense level array.
class PriceLevel {
 public:
  PriceLevel();

  void PushBack(NodeArena& arena, NodeIdx node);
  void Remove(NodeArena& arena, NodeIdx node);

  [[nodiscard]] NodeIdx Front() const noexcept;  // kNullNode when empty
  void PopFront(NodeArena& arena);

  void ReduceQty(Quantity delta) noexcept;

  [[nodiscard]] Quantity TotalQty() const noexcept;
  [[nodiscard]] std::size_t OrderCount() const noexcept;
  [[nodiscard]] bool Empty() const noexcept;

 private:
  Quantity m_total_qty;
  std::uint32_t m_order_count;
  NodeIdx m_head;
  NodeIdx m_tail;
};

}  // namespace lfob

#endif // PRICE_LEVEL_HPP_
