#ifndef ORDER_INDEX_HPP_
#define ORDER_INDEX_HPP_

#include <cstddef>
#include <cstdint>
#include <vector>
#include "types.hpp"

namespace lfob {

// OrderId -> NodeRef map for the matching thread. Owned exclusively by the
// book: no atomics, and *no runtime allocation*
//
// Replaces std::unordered_map, which allocates one node per insert via
// operator new. That put a heap allocation on the hot matching path for every
// resting order; the occasional allocation that tripped a slow kernel page
// path (transparent-hugepage fault, zeroing, reclaim) froze the thread for
// ~80us, which is exactly what showed up as the latency-tail max.
//
// Fixed-capacity open addressing with linear probing. Storage is one flat
// vector sized once at construction to the next power of two >= 2*capacity, so
// the load factor never exceeds 0.5: probe chains stay short and the table
// never grows or rehashes. Deletion uses backward-shift, so an
// endless insert/erase stream -- orders resting and cancelling forever -- never
// degrades the table or reclaims a single byte.
class OrderIndex {
 public:
  explicit OrderIndex(std::size_t capacity) {
    std::size_t n{1};
    while (n < capacity * 2) {
      n <<= 1U;
    }
    m_mask = n - 1;
    // resize value-initialises each Slot to empty (ref.idx == k_null_node) and
    // first-touches every page now, off the hot path
    m_slots.resize(n);
  }

  // Insert id->ref, or overwrite ref if id already present.
  void Put(OrderId id, NodeRef ref) noexcept {
    std::size_t i{Hash(id) & m_mask};
    while (Occupied(i)) {
      if (SlotAt(i).id == id) {
        SlotAt(i).ref = ref;
        return;
      }
      i = (i + 1) & m_mask;
    }
    SlotAt(i) = Slot{.id = id, .ref = ref};
    ++m_size;
  }

  // Pointer to the stored ref, or nullptr if no order
  [[nodiscard]] const NodeRef* Find(OrderId id) const noexcept {
    std::size_t i{Hash(id) & m_mask};
    while (Occupied(i)) {
      if (SlotAt(i).id == id) {
        return &SlotAt(i).ref;
      }
      i = (i + 1) & m_mask;
    }
    return nullptr;
  }

  // Backward-shift deletion: pull forward any entry that
  // probed past the hole and still hashes at or
  // before it, so no lookup is ever cut short by a gap.
  void Erase(OrderId id) noexcept {
    std::size_t i{Hash(id) & m_mask};
    while (Occupied(i) && SlotAt(i).id != id) {
      i = (i + 1) & m_mask;
    }
    if (!Occupied(i)) {
      // if we break because the slot wasn't occupied return
      return;
    }
    --m_size;

    std::size_t j{i};
    while (true) {
      SlotAt(i).ref.idx = k_null_node;  // open a hole at i
      while (true) {
        j = (j + 1) & m_mask;
        if (!Occupied(j)) {
          return;  // reached a gap - finished
        }
        const std::size_t k{Hash(SlotAt(j).id) & m_mask};
        // If j's ideal slot k is NOT cyclically in (i, j], j is allowed to
        // move back into the hole at i.
        if (!CyclicIn(i, k, j)) {
          break;
        }
      }
      SlotAt(i) = SlotAt(j);  // fill the hole; the hole moves to j
      i = j;
    }
  }

  [[nodiscard]] std::size_t Size() const noexcept { return m_size; }

 private:
  struct Slot {
    OrderId id{0};
    NodeRef ref{.idx = k_null_node, .gen = 0};
  };

  [[nodiscard]] bool Occupied(std::size_t i) const noexcept {
    return SlotAt(i).ref.idx != k_null_node;
  }

  // Mix so that sequential ids don't pile into one run
  // splitmix64 finaliser
  [[nodiscard]] static std::size_t Hash(OrderId id) noexcept {
    std::uint64_t x{id};
    x ^= x >> 30U;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27U;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31U;
    return static_cast<std::size_t>(x);
  }

  [[nodiscard]] static bool CyclicIn(std::size_t i,
                                     std::size_t k,
                                     std::size_t j) noexcept {
    return (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
  }

  // <---- unchecked accessors ---->
  // i is always masked by m_mask, so it is in range by construction
  [[nodiscard]] Slot& SlotAt(std::size_t i) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return m_slots[i];
  }
  [[nodiscard]] const Slot& SlotAt(std::size_t i) const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return m_slots[i];
  }

  std::vector<Slot> m_slots;
  std::size_t m_mask{0};
  std::size_t m_size{0};
};

}  // namespace lfob

#endif  // ORDER_INDEX_HPP_
