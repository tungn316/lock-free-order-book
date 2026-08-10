#ifndef NODE_ARENA_HPP_
#define NODE_ARENA_HPP_

#include <cstddef>
#include <vector>
#include "order.hpp"
#include "types.hpp"

namespace lfob {

// Index-based object pool. Owned exclusively by the matching thread:
// no atomics, no runtime allocation after reserve().
//
// Storage is one flat std::vector<Order> sized at startup. Allocation
// pops an index off free_; release pushes it back and bumps that slot's
// generation so stale NodeRefs are detectable.
class NodeArena {
 public:
  explicit NodeArena(std::size_t capacity);

  // O(1): pop_back off free_. Returns kNullNode-tagged ref when
  // exhausted — the caller must reject the order, never grow.
  NodeRef Acquire() noexcept;

  // O(1): bump generation, push index onto free_.
  void Release(NodeIdx idx) noexcept;

  // Unchecked access. Hot path; the matcher has already validated.
  Order& operator[](NodeIdx idx) noexcept;
  const Order& operator[](NodeIdx idx) const noexcept;

  // Checked access: nullptr if idx is stale or out of range.
  Order* Resolve(NodeRef ref) noexcept;

  [[nodiscard]] Generation GenerationOf(NodeIdx idx) const noexcept;

  [[nodiscard]] std::size_t Capacity() const noexcept;
  [[nodiscard]] std::size_t InUse() const noexcept;
  [[nodiscard]] bool Exhausted() const noexcept;

 private:
  std::vector<Order> m_nodes;
  std::vector<Generation> m_generations;
  std::vector<NodeIdx> m_free;  // stack; back() is the next allocation
};

}  // namespace lfob

#endif // NODE_ARENA_HPP_
