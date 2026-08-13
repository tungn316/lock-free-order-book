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
  void PushBack(NodeArena& arena, NodeIdx node) {
    Order& order = arena.Node(node);
    order.next = k_null_node;
    order.prev = m_tail;

    if (m_tail != k_null_node) {
      arena.Node(m_tail).next = node;
    } else {
      m_head = node;
    }
    m_tail = node;

    m_total_quantity += order.remaining;
    ++m_order_count;
  }

  void Remove(NodeArena& arena, NodeIdx node) {
    Order& order = arena.Node(node);

    if (order.prev != k_null_node) {
      arena.Node(order.prev).next = order.next;
    } else {
      m_head = order.next;
    }

    if (order.next != k_null_node) {
      arena.Node(order.next).prev = order.prev;
    } else {
      m_tail = order.prev;
    }

    order.prev = k_null_node;
    order.next = k_null_node;

    m_total_quantity -= order.remaining;
    --m_order_count;
  }

  [[nodiscard]] NodeIdx Front() const noexcept { return m_head; }

  void PopFront(NodeArena& arena) { Remove(arena, m_head); }

  [[nodiscard]] Quantity TotalQuantity() const noexcept {
    return m_total_quantity;
  }
  [[nodiscard]] std::size_t OrderCount() const noexcept {
    return m_order_count;
  }
  [[nodiscard]] bool Empty() const noexcept { return m_order_count == 0; }

 private:
  Quantity m_total_quantity{0};
  std::uint32_t m_order_count{0};
  NodeIdx m_head{k_null_node};
  NodeIdx m_tail{k_null_node};
};

}  // namespace lfob

#endif  // PRICE_LEVEL_HPP_
