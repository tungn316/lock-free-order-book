#ifndef NODE_ARENA_HPP_
#define NODE_ARENA_HPP_

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <numeric>
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
  // No need to prefault as resize value initialises
  explicit NodeArena(std::size_t capacity) {
    m_nodes.resize(capacity);
    m_generations.resize(capacity, 0);
    m_free.resize(capacity);
    std::ranges::iota(m_free, 0);
    std::ranges::reverse(m_free);
  }

  // O(1): pop_back off m_free
  // Returns kNullNode-tagged ref when exhausted
  NodeRef Acquire() noexcept {
    if (m_free.empty()) {
      return {.idx = k_null_node, .gen = 0};
    }
    const NodeIdx idx{m_free.back()};
    m_free.pop_back();
    return {.idx = idx, .gen = GenAt(idx)};
  }

  // O(1): bump generation, push index onto m_free
  void Release(NodeIdx idx) noexcept {
    assert(idx < m_nodes.size());
    m_free.push_back(idx);
    ++GenAt(idx);
  }

  // <---- UNCHECKED ACCESSORS ---->
  // Matching Engine already validated - Invariant: Pulled off m_free

  Order& operator[](NodeIdx idx) noexcept { return NodeAt(idx); }
  const Order& operator[](NodeIdx idx) const noexcept { return NodeAt(idx); }

  // Same as operator[] but named to make my LSP happy
  [[nodiscard]] Order& Node(NodeIdx idx) noexcept { return NodeAt(idx); }
  [[nodiscard]] const Order& Node(NodeIdx idx) const noexcept { return NodeAt(idx); }

  // <---- CHECKED ACCESOSR ---->
  // return nullptr if idx is stale or out of range
  Order* Resolve(NodeRef ref) noexcept {
    if (ref.idx >= m_generations.size()) {
      return nullptr;
    }
    if (ref.gen == GenAt(ref.idx)) {
      return &NodeAt(ref.idx);
    }
    return nullptr;
  }

  [[nodiscard]] Generation GenerationOf(NodeIdx idx) const noexcept { return GenAt(idx); }
  [[nodiscard]] std::size_t Capacity() const noexcept { return (m_nodes.size()); }
  [[nodiscard]] std::size_t InUse() const noexcept { return m_nodes.size() - m_free.size(); }
  [[nodiscard]] bool Exhausted() const noexcept { return m_free.empty(); }

 private:
  std::vector<Order> m_nodes;
  std::vector<Generation> m_generations;
  std::vector<NodeIdx> m_free;

  // ── unchecked accessors ────────────────────────────────────────────────
  // Every raw index into the parallel vectors funnels through here. The
  // arena's own invariant guarantees idx is in range (it came off m_free),
  // so the bounds check is deliberately skipped. Suppression lives in ONE
  // place instead of being scattered across the class.
  //
  // tldr: make clangd happy

  [[nodiscard]] Order& NodeAt(NodeIdx idx) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return m_nodes[idx];
  }
  [[nodiscard]] const Order& NodeAt(NodeIdx idx) const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return m_nodes[idx];
  }
  [[nodiscard]] Generation& GenAt(NodeIdx idx) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return m_generations[idx];
  }
  [[nodiscard]] Generation GenAt(NodeIdx idx) const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return m_generations[idx];
  }
};

}  // namespace lfob

#endif  // NODE_ARENA_HPP_
