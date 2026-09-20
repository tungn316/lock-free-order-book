#ifndef MPSC_QUEUE_HPP_
#define MPSC_QUEUE_HPP_

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>
#include "types.hpp"

namespace lfob {

// Bounded lock-free MPSC ring
//
// Flat array of size CAPACITY index each holding a 'Slot'
// Each 'Slot' holds a ready value and the payload
// For each 'Slot' i in [0, CAPACITY - 1] if:
// - Ready = i + (CAPACITY * n) where n in the cycle count i.e. how many times we have looped
// This means 'Slot' is ready to be written to
// - Ready = (i + (CAPACITY * n)) + 1
// This means it has been written to and is waiting to be read
//
// Each slot needs to be read only once, lossless, on a full ring we return false to the caller
// applying backpressure

template <typename T, std::size_t CAPACITY>
class MpscRing {
  static_assert((CAPACITY & (CAPACITY - 1)) == 0,
                "Capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  MpscRing() noexcept {
    // Starting condition - for all m_buffer[i], ready must be set to i to indicate ready
    for (auto i{0UZ}; i < CAPACITY; ++i) {
      SlotAt(i).ready.store(static_cast<SeqNum>(i), std::memory_order::relaxed);
    }
  }
  ~MpscRing() noexcept = default;
  MpscRing(const MpscRing&) = delete;
  MpscRing(MpscRing&&) = delete;
  MpscRing& operator=(const MpscRing&) = delete;
  MpscRing& operator=(MpscRing&&) = delete;

  bool TryPush(const T& item) noexcept {
    std::size_t tail{m_tail.load(std::memory_order::relaxed)};

    for (;;) {
      Slot& slot{SlotAt(tail)};
      const SeqNum ready{slot.ready.load(std::memory_order::acquire)};
      const SeqNum expected{static_cast<SeqNum>(tail)};
      const auto dif{static_cast<std::make_signed_t<SeqNum>>(ready - expected)};

      // ready == tail -> ready to be written to
      if (dif == 0) {
        if (m_tail.compare_exchange_weak(tail, tail + 1,
                                         std::memory_order::relaxed,
                                         std::memory_order::relaxed)) {
          slot.payload = item;
          slot.ready.store(expected + 1, std::memory_order::release);
          return true;
        }
      }
      // Tail has hit a ready that is at least CAPACITY away from head
      // Ready hasn't incremented by CAPACITY to be included in the next cycle
      // Apply backpressure
      else if (dif < 0) {
        return false;
      }
      // stale tail, reload and try again
      // if ready > expected we either:
      // - increased by 1 indicating written already and ready to be read
      // - increased by atleast CAPACITY so ready to be written again
      else {
        tail = m_tail.load(std::memory_order::relaxed);
      }
    }
  }

  std::size_t TryPushBulk(const T* items, std::size_t count) noexcept {
    if (count == 0) {
      return 0;
    }
    std::size_t tail{m_tail.load(std::memory_order::relaxed)};

    // TryPush() but with a lot of items
    for (;;) {
      std::size_t n{0};
      while (n < count) {
        const Slot& slot{SlotAt(tail + n)};
        const SeqNum ready{slot.ready.load(std::memory_order::acquire)};
        const auto dif = static_cast<std::make_signed_t<SeqNum>>(
            ready - static_cast<SeqNum>(tail + n));
        if (dif != 0) {
          break;
        }
        ++n;
      }
      if (n == 0) {
        return 0;
      }

      if (m_tail.compare_exchange_weak(tail, tail + n,
                                       std::memory_order::relaxed,
                                       std::memory_order::relaxed)) {
        for (auto i{0UZ}; i < n; ++i) {
          Slot& slot{SlotAt(tail + i)};
          const SeqNum expected{static_cast<SeqNum>(tail + i)};
          slot.payload = items[i];
          slot.ready.store(expected + 1, std::memory_order::release);
        }
        return n;
      } else {
        continue;
      }
    }
  }

  std::optional<T> TryPop() noexcept {
    Slot& slot{SlotAt(m_head)};
    const SeqNum ready{slot.ready.load(std::memory_order::acquire)};
    const SeqNum expected{static_cast<SeqNum>(m_head) + 1};

    // slot.ready = (m_head + 1) -> has been written to
    if (ready == expected) {
      T value{slot.payload};
      slot.ready.store(static_cast<SeqNum>(m_head) + CAPACITY,
                       std::memory_order::release);
      ++m_head;
      return value;
    }

    else {
      return std::nullopt;
    }
  }

  std::size_t TryPopBulk(T* out, std::size_t max) noexcept {
    std::size_t n{0};
    for (auto i{0UZ}; i < max; ++i) {
      Slot& slot{SlotAt(m_head)};
      const SeqNum ready{slot.ready.load(std::memory_order::acquire)};
      const SeqNum expected{static_cast<SeqNum>(m_head) + 1};

      // slot.ready = (m_head + 1) -> has been written to
      if (ready == expected) {
        ++n;
        out[i] = slot.payload;
        slot.ready.store(static_cast<SeqNum>(m_head) + CAPACITY,
                         std::memory_order::release);
        ++m_head;
      } else {
        break;
      }
    }
    return n;
  }

  // Approximate deapth, only used by consumer!
  [[nodiscard]] std::size_t SizeApprox() const noexcept {
    return (m_tail.load(std::memory_order::relaxed) - m_head);
  }

 private:
  struct alignas(k_cache_line) Slot {
    std::atomic<SeqNum> ready;
    T payload{};
  };


  // ── unchecked accessors ────────────────────────────────────────────────
  // Every raw index into m_buffer funnels through here. The
  // bounds check is deliberately skipped. Suppression lives in ONE place
  // instead of being scattered across every push/pop.
  [[nodiscard]] Slot& SlotAt(std::size_t seq) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
    return m_buffer[seq & (CAPACITY - 1)];
  }

  alignas(k_cache_line) std::atomic<std::size_t> m_tail{0};
  alignas(k_cache_line) std::size_t m_head{0};
  alignas(k_cache_line) std::array<Slot, CAPACITY> m_buffer;
};

}  // namespace lfob

#endif  // MPSC_QUEUE_HPP_
