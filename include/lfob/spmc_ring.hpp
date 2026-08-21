#ifndef SPMC_RING_HPP_
#define SPMC_RING_HPP_

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include "types.hpp"

namespace lfob {

// Single Producer Multi Consumer Broadcast Ring
//
// Every registered consumer observes every item. The producer publishes a
// single monotonically increasing sequence, each consumer reads with it's own cursor.
//
// Delivery is lossless with back-pressure. The producer may not overwrite a
// slot until the slowest registered consumer has moved past it, so TryPush
// fails when the ring is full.
//
// Consumers must be handed out via Register() before Start() / the first
// publish

template <typename T, std::size_t CAPACITY, std::size_t MAX_CONSUMERS = 8>
class SpmcRing {
  static_assert((CAPACITY & (CAPACITY - 1)) == 0,
                "Capacity must be a power of two");
  static_assert(MAX_CONSUMERS >= 1);
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  struct ConsumerId {
    std::uint32_t idx;
  };

  SpmcRing() noexcept = default;
  ~SpmcRing() noexcept = default;
  SpmcRing(const SpmcRing&) = delete;
  SpmcRing(SpmcRing&&) = delete;
  SpmcRing& operator=(const SpmcRing&) = delete;
  SpmcRing& operator=(SpmcRing&&) = delete;

  // <---- setup: single-threaded, before Start() ---->

  // Hand out an independent read cursor
  [[nodiscard]] ConsumerId Register() noexcept {
    const std::size_t id{m_consumer_count};
    assert(id < MAX_CONSUMERS && "too many consumers");
    if (id >= MAX_CONSUMERS) {
      return ConsumerId{k_invalid_consumer};
    }
    CursorAt(id).store(0, std::memory_order::relaxed);
    ++m_consumer_count;
    return ConsumerId{static_cast<std::uint32_t>(id)};
  }

  // <---- producer: matching thread only ---->

  bool TryPush(const T& item) noexcept {
    const SeqNum seq{m_produced.load(std::memory_order::relaxed)};

    // Writing seq reuses the slot currently holding (seq - CAPACITY)
    // The cache is <= the true min, so acting on it can only trigger
    // a needless re-scan, never an unsafe overwrite
    if (seq - m_gate_cache >= CAPACITY) {
      m_gate_cache = MinCursor(seq);
      if (seq - m_gate_cache >= CAPACITY) {
        return false;
      }
    }

    SlotAt(seq).payload = item;
    // Release: publishes the payload write above to any acquiring consumer
    m_produced.store(seq + 1, std::memory_order::release);
    return true;
  }

  std::size_t TryPushBulk(const T* items, std::size_t count) noexcept {
    if (count == 0) {
      return 0;
    }
    const SeqNum seq{m_produced.load(std::memory_order::relaxed)};

    // Clamp before subtracting: a stale cache can leave (used > CAPACITY)
    std::size_t used{static_cast<std::size_t>(seq - m_gate_cache)};
    std::size_t headroom{std::max(CAPACITY - used, 0UZ)};
    // Only if the worst case scenario doesn't have enough space we reload m_gate_cache
    if (count > headroom) {
      m_gate_cache = MinCursor(seq);
      used = static_cast<std::size_t>(seq - m_gate_cache);
      headroom = std::max(CAPACITY - used, 0UZ);
    }
    if (headroom == 0) {
        return 0;
    }

    const std::size_t n{std::min(count, headroom)};
    for (auto i{0UZ}; i < n; ++i) {
      SlotAt(seq + i).payload = items[i];
    }
    // Single release store publishes the whole batch at once
    m_produced.store(seq + n, std::memory_order::release);
    return n;
  }

  // <---- consumers: each id polled on its own thread ---->

  std::optional<T> TryRead(ConsumerId id) noexcept {
    const std::size_t i{id.idx};
    assert(i < m_consumer_count);
    const SeqNum cursor{CursorAt(i).load(std::memory_order::relaxed)};
    // Acquire: pairs with the producer's release store, making the payload for
    // [cursor, produced) visible before we copy it
    const SeqNum produced{m_produced.load(std::memory_order::acquire)};
    if (cursor == produced) {
      return std::nullopt;
    }

    T value{SlotAt(cursor).payload};
    // Release: our payload read above happens-before this store, so the
    // producer will not overwrite the slot until we are done with it, paired
    // in MinCursor
    CursorAt(i).store(cursor + 1, std::memory_order::release);
    return value;
  }

  std::size_t TryReadBulk(ConsumerId id, T* out, std::size_t max) noexcept {
    const std::size_t i{id.idx};
    assert(i < m_consumer_count);
    const SeqNum cursor{CursorAt(i).load(std::memory_order::relaxed)};
    const SeqNum produced{m_produced.load(std::memory_order::acquire)};

    std::size_t avail{static_cast<std::size_t>(produced - cursor)};
    avail = std::min(avail, max);
    if (avail == 0) {
      return 0;
    }

    for (auto k{0UZ}; k < avail; ++k) {
      out[k] = SlotAt(cursor + k).payload;
    }
    CursorAt(i).store(cursor + avail, std::memory_order::release);
    return avail;
  }

  // <---- monitoring only ---->

  // Backlog of a single consumer (published sequence minus its cursor).
  [[nodiscard]] std::size_t LagOf(ConsumerId id) const noexcept {
    const SeqNum produced{m_produced.load(std::memory_order::relaxed)};
    const SeqNum cursor =
        CursorAt(id.idx).load(std::memory_order::relaxed);
    return static_cast<std::size_t>(produced - cursor);
  }

  // Backlog of the slowest consumer == how full the ring is.
  [[nodiscard]] std::size_t SizeApprox() const noexcept {
    const SeqNum produced{m_produced.load(std::memory_order::relaxed)};
    return static_cast<std::size_t>(produced - MinCursor(produced));
  }

 private:
  // Smallest cursor across registered consumers.
  // Acquire loads: required on the overwrite-authorizing path so a
  // consumer's payload read happens-before the producer's reuse of that slot
  [[nodiscard]] SeqNum MinCursor(SeqNum fallback) const noexcept {
    if (m_consumer_count == 0) {
      return fallback;
    }
    SeqNum m{CursorAt(0).load(std::memory_order::acquire)};
    for (auto i{1UZ}; i < m_consumer_count; ++i) {
      const SeqNum c{CursorAt(i).load(std::memory_order::acquire)};
      m = std::min(c, m);
    }
    return m;
  }

  struct alignas(k_cache_line) CursorCell {
    std::atomic<SeqNum> cursor{0};
  };

  struct alignas(k_cache_line) Slot {
    T payload{};
  };

  // <---- unchecked accessors ---->
  // Every raw index into m_buffer funnels through here. The
  // bounds check is deliberately skipped. Suppression lives in ONE place
  // instead of being scattered across every push/pop.
  [[nodiscard]] Slot& SlotAt(SeqNum seq) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
    return m_buffer[seq & (CAPACITY - 1)];
  }
  [[nodiscard]] std::atomic<SeqNum>& CursorAt(std::size_t idx) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
    return m_cursors[idx].cursor;
  }
  [[nodiscard]] const std::atomic<SeqNum>& CursorAt(
      std::size_t idx) const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
    return m_cursors[idx].cursor;
  }

  // Producer-write / consumer-read boundary; alone on its own line
  alignas(k_cache_line) std::atomic<SeqNum> m_produced{0};

  alignas(k_cache_line) SeqNum m_gate_cache{0};
  std::size_t m_consumer_count{0};

  // One padded cursor per consumer so no false sharing between readers or with
  // the producer's gate scan
  alignas(k_cache_line) std::array<CursorCell, MAX_CONSUMERS> m_cursors{};

  // Padded slots so a producer store to slot k never shares a line with a
  // consumer load from slot k-1. Storing to slot k dirties the entire cache line
  // meaning consumers must reload the cache just to read unchanged bytes
  //
  // Every slot line is pulled into all N reader caches so not good so up to N dirtied caches
  // every write
  alignas(k_cache_line) std::array<Slot, CAPACITY> m_buffer{};
};

}  // namespace lfob

#endif  // SPMC_RING_HPP_
