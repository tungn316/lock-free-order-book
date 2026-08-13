#ifndef SPMC_RING_HPP_
#define SPMC_RING_HPP_

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>
#include "types.hpp"

namespace lfob {

inline constexpr std::size_t k_max_consumers = 8;

// SPMC BROADCAST ring.
//
// Every consumer sees every message. The producer (matching thread)
// writes slot[seq & mask] and release-stores tail_. Each consumer owns
// a private cursor on its own cache line; the producer only reads those
// cursors to check it is not about to overwrite unread data.
//
// A consumer that falls behind by more than Capacity is *dropped* (its
// cursor is force-advanced and it is told how many it lost) rather than
// being allowed to stall the engine. The matcher must never block on a
// slow network thread.
template <typename T, std::size_t CAPACITY>
class SpmcRing {
  static_assert((CAPACITY & (CAPACITY - 1)) == 0,
                "Capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  SpmcRing() noexcept {
    for (auto i{0UZ}; i < CAPACITY; ++i) {
      m_buffer[i].ready.store(static_cast<SeqNum>(i),
                              std::memory_order::relaxed);
    }
  }

  // Handle a consumer uses to read; index into m_cursors.
  using ConsumerId = std::size_t;

  // Called once at startup, before the engine thread starts.
  [[nodiscard]] std::optional<ConsumerId> RegisterConsumer() {
    std::size_t tail = m_tail.load(std::memory_order::acquire);
    ConsumerId id = m_consumer_count.fetch_add(1, std::memory_order::relaxed);

    if (id >= k_max_consumers) {
      m_consumer_count.fetch_sub(1, std::memory_order::relaxed);
      return std::nullopt;
    }

    m_cursors[id].pos.store(tail, std::memory_order_relaxed);
    return id;
  }

  // --- Producer: matching thread only ---
  // Never fails, never blocks. Overwrites the oldest slot if a
  // consumer is too slow; that consumer observes a gap.
  void Publish(const T& item) noexcept {
    const std::size_t count = m_consumer_count.load(std::memory_order::acquire);

    // Force advance consumers that would be overwritten
    for (auto i{0UZ}; i < count; ++i) {
      std::size_t cur = m_cursors[i].pos.load(std::memory_order::acquire);
      if (m_producer_pos - cur >= CAPACITY) {
        m_cursors[i].pos.store(m_producer_pos - CAPACITY + 1,
                               std::memory_order::release);
        m_cursors[i].missed.fetch_add(m_producer_pos - cur - CAPACITY + 1,
                                      std::memory_order::relaxed);
      }
    }

    Slot& slot = m_buffer[m_producer_pos & (CAPACITY - 1)];
    slot.payload = item;
    slot.ready.store(static_cast<SeqNum>(m_producer_pos + 1),
                     std::memory_order::release);
    m_tail.store(m_producer_pos + 1, std::memory_order::release);
    m_producer_pos++;
  }

  // Reserve a slot and write in place, avoiding a copy for large T.
  [[nodiscard]] T& Reserve() noexcept {
    const std::size_t count = m_consumer_count.load(std::memory_order_acquire);

    for (auto i{0UZ}; i < count; ++i) {
      std::size_t cur = m_cursors[i].pos.load(std::memory_order_acquire);
      if (m_producer_pos - cur >= CAPACITY) {
        m_cursors[i].pos.store(m_producer_pos - CAPACITY + 1,
                               std::memory_order::release);
        m_cursors[i].missed.fetch_add(m_producer_pos - cur - CAPACITY + 1,
                                      std::memory_order::relaxed);
      }
    }

    Slot& slot = m_buffer[m_producer_pos & (CAPACITY - 1)];
    return slot.payload;
  }

  void Commit() noexcept {
    Slot& slot = m_buffer[m_producer_pos & (CAPACITY - 1)];
    slot.ready.store(static_cast<SeqNum>(m_producer_pos + 1),
                     std::memory_order::release);
    m_tail.store(m_producer_pos + 1, std::memory_order::release);
    m_producer_pos++;
  }

  // --- Consumers: each network output thread ---
  [[nodiscard]] std::optional<T> TryRead(ConsumerId id) noexcept {
    for (;;) {
      std::size_t cur = m_cursors[id].pos.load(std::memory_order::acquire);
      std::size_t tail = m_tail.load(std::memory_order::acquire);
      if (cur >= tail) {
        return std::nullopt;
      }

      Slot& slot = m_buffer[cur & (CAPACITY - 1)];
      const SeqNum expected = static_cast<SeqNum>(cur + 1);

      // Seqlock read of this slot.
      SeqNum r1 = slot.ready.load(std::memory_order::acquire);
      T item = slot.payload;
      std::atomic_thread_fence(std::memory_order::acquire);
      SeqNum r2 = slot.ready.load(std::memory_order::acquire);

      if (r1 != r2) {
        continue;  // torn: producer mid-write, spin
      }
      if (r1 != expected) {
        // r1 > expected: lapped. Producer already force-advanced our
        // cursor, but reload to be safe and retry with the new cur.
        continue;  // outer loop reloads cur/tail
      }

      m_cursors[id].pos.store(cur + 1, std::memory_order::release);
      return item;
    }
  }

  // Batched read; returns count copied into out.
  std::size_t TryReadBulk(ConsumerId id, T* out, std::size_t max) noexcept {
    std::size_t cur = m_cursors[id].pos.load(std::memory_order::acquire);
    std::size_t n{0};

    while (n < max) {
      std::size_t tail = m_tail.load(std::memory_order::acquire);
      if (cur >= tail) {
        break;
      }

      Slot& slot = m_buffer[cur & (CAPACITY - 1)];
      const SeqNum expected = static_cast<SeqNum>(cur + 1);

      // Seqlock read of this slot
      SeqNum r1 = slot.ready.load(std::memory_order::acquire);
      T item = slot.payload;
      std::atomic_thread_fence(std::memory_order::acquire);
      SeqNum r2 = slot.ready.load(std::memory_order::acquire);

      if (r1 != r2) {
        continue;  // torn: producer mid-write, spin on same cur
      }
      if (r1 != expected) {
        // lapped: producer force-advanced us. Reload cursor and retry.
        cur = m_cursors[id].pos.load(std::memory_order::acquire);
        continue;
      }

      out[n] = item;
      ++cur;
      ++n;
    }

    m_cursors[id].pos.store(cur, std::memory_order::release);
    return n;
  }

  // Messages this consumer missed because it fell behind. Reset on read.
  [[nodiscard]] std::uint64_t Missed(ConsumerId id) noexcept {
    return m_cursors[id].missed.exchange(0, std::memory_order::relaxed);
  }

  [[nodiscard]] std::size_t Lag(ConsumerId id) const noexcept {
    return (m_tail.load(std::memory_order::relaxed) -
            m_cursors[id].pos.load(std::memory_order::relaxed));
  }

 private:
  struct alignas(k_cache_line) Cursor {
    std::atomic<std::size_t> pos{0};
    std::atomic<std::uint64_t> missed{0};
  };

  struct alignas(k_cache_line) Slot {
    std::atomic<SeqNum> ready;
    T payload;
  };

  alignas(k_cache_line) std::atomic<std::size_t> m_tail{0};
  alignas(k_cache_line) std::size_t m_producer_pos{0};  // private to producer
  alignas(k_cache_line) std::atomic<std::size_t> m_consumer_count{0};

  std::array<Cursor, k_max_consumers> m_cursors;
  alignas(k_cache_line) std::array<Slot, CAPACITY> m_buffer;
};

}  // namespace lfob

#endif  // SPMC_RING_HPP_
