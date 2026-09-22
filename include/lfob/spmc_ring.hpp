#ifndef SPMC_RING_HPP_
#define SPMC_RING_HPP_

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>
#include "types.hpp"

namespace lfob {

// Single Producer Multi Consumer Broadcast Ring
//
// Every registered consumer observes every item. The producer publishes a
// single monotonically increasing sequence, each consumer reads with it's own
// cursor.
//
// Delivery is lossless with back-pressure. The producer may not overwrite a
// slot until the slowest registered consumer has moved past it, so TryPush
// fails when the ring is full.
//
// Consumers must be handed out via Register() before Start() / the first
// publish
//
// <---- Detaching a dead consumer ---->
//
// Back-pressure is only sound while every consumer is alive. A consumer that
// stops draining -- crashed thread, wedged in a syscall, starved by the
// scheduler -- pins the gate forever and the producer never publishes again.
// On the egress ring that means one dead output worker freezes the matching
// thread, which then backs up into ingress and rejects every client. So the
// producer must be able to throw a consumer overboard.
//
// Evict() detaches a consumer: its cursor stops gating and the producer
// reclaims its slots immediately. That deliberately races the victim, which
// may be mid-copy, so cell liveness is versioned rather than boolean:
//
//   epoch odd  -> attached, the cursor gates the producer
//   epoch even -> detached, the cursor is ignored
//
// Every transition bumps the epoch, and a consumer re-reads it *after*
// copying a batch. Evict() publishes the new epoch (release) before the
// producer reclaims a single slot, so a consumer that copied torn bytes is
// guaranteed to observe the change and drop them on the floor. A detached
// consumer is therefore silent, never wrong: it reads nothing until it
// Rejoin()s, and it rejoins at the live edge.
//
// The copy performed by a doomed consumer does still race the producer's
// overwrite. That race is unavoidable against a reader that may be suspended
// mid-copy -- the only alternative is the indefinite stall we are removing --
// and it is contained: T is trivially copyable, the bytes land in the
// consumer's own buffer, and the post-copy epoch check discards them before
// the caller ever sees them.

template <typename T, std::size_t CAPACITY, std::size_t MAX_CONSUMERS = 8>
class SpmcRing {
  static_assert((CAPACITY & (CAPACITY - 1)) == 0,
                "Capacity must be a power of two");
  static_assert(MAX_CONSUMERS >= 1);
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

 public:
  struct ConsumerId {
    std::uint32_t idx;
  };

  // Registration liveness, versioned. Parity is the state, the value is the
  // generation: a consumer that is evicted and rejoins gets a fresh epoch, so
  // "still attached" is distinguishable from "attached again"
  using Epoch = std::uint64_t;

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
    EpochAt(id).store(1, std::memory_order::relaxed);  // odd == attached
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

    SlotAt(seq).Store(item);
    // Release: publishes the payload write above to any acquiring consumer
    m_produced.store(seq + 1, std::memory_order::release);
    return true;
  }

  std::size_t TryPushBulk(const T* items, std::size_t count) noexcept {
    if (count == 0) {
      return 0;
    }
    const SeqNum seq{m_produced.load(std::memory_order::relaxed)};

    std::size_t headroom{Headroom(seq)};
    // Only if the worst case scenario doesn't have enough space we reload
    // m_gate_cache
    if (count > headroom) {
      m_gate_cache = MinCursor(seq);
      headroom = Headroom(seq);
    }
    if (headroom == 0) {
      return 0;
    }

    const std::size_t n{std::min(count, headroom)};
    for (auto i{0UZ}; i < n; ++i) {
      SlotAt(seq + i).Store(items[i]);
    }
    // Single release store publishes the whole batch at once
    m_produced.store(seq + n, std::memory_order::release);
    return n;
  }

  // Producer only: stop honouring this consumer's cursor so its slots can be
  // reclaimed now. Returns false if it was already detached (evicted earlier,
  // or it left on its own).
  //
  // The release CAS lands before the gate is recomputed, and the gate is what
  // authorises the first overwrite, so the victim always sees the new epoch on
  // the validation that follows its copy
  bool Evict(ConsumerId id) noexcept {
    if (!Detach(id)) {
      return false;
    }
    m_evictions.fetch_add(1, std::memory_order::relaxed);
    m_gate_cache = MinCursor(m_produced.load(std::memory_order::relaxed));
    return true;
  }

  // <---- consumers: each id polled on its own thread ---->

  // A return of nullopt / 0 means "nothing right now":
  // - either ring is drained or this consumer has been detached
  // - Callers that care about the difference check IsAttached()
  std::optional<T> TryRead(ConsumerId id) noexcept {
    T value{};
    if (TryReadBulk(id, &value, 1) == 0) {
      return std::nullopt;
    }
    return value;
  }

  std::size_t TryReadBulk(ConsumerId id, T* out, std::size_t max) noexcept {
    const std::size_t i{id.idx};
    assert(i < m_consumer_count);

    const Epoch epoch{EpochAt(i).load(std::memory_order::acquire)};
    if (!Attached(epoch)) {
      return 0;
    }

    const SeqNum cursor{CursorAt(i).load(std::memory_order::relaxed)};
    // Acquire: pairs with the producer's release store, making the payload for
    // [cursor, produced) visible before we copy it
    const SeqNum produced{m_produced.load(std::memory_order::acquire)};
    const std::size_t avail{
        std::min(static_cast<std::size_t>(produced - cursor), max)};
    if (avail == 0) {
      return 0;
    }

    for (auto k{0UZ}; k < avail; ++k) {
      out[k] = SlotAt(cursor + k).Load();
    }

    if (!Validate(i, epoch, cursor)) {
      return 0;
    }
    // Release: our payload read above happens-before this store, so the
    // producer will not overwrite the slot until we are done with it, paired
    // in MinCursor
    CursorAt(i).store(cursor + avail, std::memory_order::release);
    return avail;
  }

  // Consumer only, after observing IsAttached() == false. Re-attaches at the
  // live edge: everything published while detached is gone for good, which is
  // the point. A worker that fell far enough behind to be evicted wants a
  // fresh snapshot and a gap notice, not a backlog of stale quotes to replay.
  //
  // Returns false if the consumer is already attached
  bool Rejoin(ConsumerId id) noexcept {
    const std::size_t i{id.idx};
    if (i >= m_consumer_count) {
      return false;
    }
    Epoch epoch{EpochAt(i).load(std::memory_order::relaxed)};
    if (Attached(epoch)) {
      return false;
    }
    // Park first: an attached cell still carrying the cursor from a previous
    // life would gate the producer at an ancient sequence and get us evicted
    // again on the spot
    CursorAt(i).store(k_cursor_parked, std::memory_order::relaxed);
    if (!EpochAt(i).compare_exchange_strong(epoch, epoch + 1,
                                            std::memory_order::release,
                                            std::memory_order::relaxed)) {
      return false;
    }
    // Take the join point last, once we are already visible as attached: the
    // producer cannot have lapped a sequence it has not published yet
    CursorAt(i).store(m_produced.load(std::memory_order::acquire),
                      std::memory_order::release);
    return true;
  }

  // Consumer only: graceful exit. Same effect as being evicted, minus
  // eviction counter
  bool Leave(ConsumerId id) noexcept { return Detach(id); }

  [[nodiscard]] bool IsAttached(ConsumerId id) const noexcept {
    return id.idx < m_consumer_count &&
           Attached(EpochAt(id.idx).load(std::memory_order::acquire));
  }

  [[nodiscard]] Epoch EpochOf(ConsumerId id) const noexcept {
    return EpochAt(id.idx).load(std::memory_order::acquire);
  }

  // <---- monitoring only ---->

  // The attached consumer whose backlog is currently blocking the producer, or
  // k_invalid_consumer when nothing is. Only meaningful to the producer, and
  // only once TryPush has actually failed: this is the eviction candidate
  [[nodiscard]] ConsumerId BlockingConsumer() const noexcept {
    const SeqNum produced{m_produced.load(std::memory_order::relaxed)};
    SeqNum slowest{produced};
    std::uint32_t owner{k_invalid_consumer};

    for (auto i{0UZ}; i < m_consumer_count; ++i) {
      const SeqNum cursor{ActiveCursor(i, produced)};
      if (cursor < slowest) {
        slowest = cursor;
        owner = static_cast<std::uint32_t>(i);
      }
    }
    if (produced - slowest < CAPACITY) {
      return ConsumerId{k_invalid_consumer};  // full ring, but not this one
    }
    return ConsumerId{owner};
  }

  // Backlog of a single consumer (published sequence minus its cursor).
  [[nodiscard]] std::size_t LagOf(ConsumerId id) const noexcept {
    const SeqNum produced{m_produced.load(std::memory_order::relaxed)};
    return static_cast<std::size_t>(produced - ActiveCursor(id.idx, produced));
  }

  // Backlog of the slowest attached consumer == how full the ring is.
  [[nodiscard]] std::size_t SizeApprox() const noexcept {
    const SeqNum produced{m_produced.load(std::memory_order::relaxed)};
    return static_cast<std::size_t>(produced - MinCursor(produced));
  }

  [[nodiscard]] std::uint64_t Evictions() const noexcept {
    return m_evictions.load(std::memory_order::relaxed);
  }

  // Batches dropped by a still-attached consumer because the producer lapped
  // it mid-copy. Should read zero forever; anything else means the gate scan
  // missed an attached cursor and the ring is losing data silently
  [[nodiscard]] std::uint64_t Lapped() const noexcept {
    return m_lapped.load(std::memory_order::relaxed);
  }

 private:
  // Cursor of a cell that is neither attached nor positioned. MinCursor treats
  // it as "not gating", so it must sort above any real sequence
  static constexpr SeqNum k_cursor_parked{~SeqNum{0}};

  [[nodiscard]] static bool Attached(Epoch epoch) noexcept {
    return (epoch & 1U) != 0;
  }

  // Flip attached -> detached. Both ends may call this (the producer to evict,
  // the consumer to leave) so it is a CAS, not a store: whoever loses the race
  // reports that the cell was already detached instead of double-counting it
  bool Detach(ConsumerId id) noexcept {
    const std::size_t i{id.idx};
    if (i >= m_consumer_count) {
      return false;
    }
    Epoch epoch{EpochAt(i).load(std::memory_order::relaxed)};
    if (!Attached(epoch)) {
      return false;
    }
    return EpochAt(i).compare_exchange_strong(epoch, epoch + 1,
                                              std::memory_order::release,
                                              std::memory_order::relaxed);
  }

  // Post-copy check, run before the batch is handed to the caller.
  //
  // - epoch changed: we were evicted mid-copy, so the producer may have
  //   overwritten the slots we were reading. Drop the batch and leave the
  //   cursor alone; the caller sees 0 and finds out why from IsAttached()
  // - lapped: the producer got more than CAPACITY ahead of our cursor while we
  //   were still attached. Only reachable in the sliver between Rejoin()
  //   publishing our epoch and the producer's next gate scan observing our
  //   cursor. Cheap to check on an already-hot line, and the recovery is the
  //   same as eviction: drop the batch, jump to the live edge
  bool Validate(std::size_t i, Epoch epoch, SeqNum cursor) noexcept {
    if (EpochAt(i).load(std::memory_order::acquire) != epoch) {
      return false;
    }
    const SeqNum produced{m_produced.load(std::memory_order::acquire)};
    if (produced - cursor > CAPACITY) {
      m_lapped.fetch_add(1, std::memory_order::relaxed);
      CursorAt(i).store(produced, std::memory_order::release);
      return false;
    }
    return true;
  }

  // Cursor as the producer's gate sees it: k_cursor_parked for a cell that is
  // detached or mid-rejoin, so it never pins the gate
  [[nodiscard]] SeqNum ActiveCursor(std::size_t i,
                                    SeqNum fallback) const noexcept {
    if (!Attached(EpochAt(i).load(std::memory_order::acquire))) {
      return fallback;
    }
    const SeqNum cursor{CursorAt(i).load(std::memory_order::acquire)};
    return cursor == k_cursor_parked ? fallback : cursor;
  }

  // Smallest cursor across attached consumers.
  // Acquire loads: required on the overwrite-authorizing path so a
  // consumer's payload read happens-before the producer's reuse of that slot
  [[nodiscard]] SeqNum MinCursor(SeqNum fallback) const noexcept {
    SeqNum min{fallback};
    for (auto i{0UZ}; i < m_consumer_count; ++i) {
      min = std::min(ActiveCursor(i, fallback), min);
    }
    return min;
  }

  // Free slots according to the cached gate. Clamped, not wrapped: a gate that
  // moves while the cache is stale can leave (seq - cache) above CAPACITY, and
  // unsigned subtraction would turn that into near-infinite headroom
  [[nodiscard]] std::size_t Headroom(SeqNum seq) const noexcept {
    const auto used{static_cast<std::size_t>(seq - m_gate_cache)};
    return used >= CAPACITY ? 0UZ : CAPACITY - used;
  }

  struct alignas(k_cache_line) CursorCell {
    std::atomic<SeqNum> cursor{0};
    std::atomic<Epoch> epoch{0};  // even == detached; a fresh cell is unowned
  };

  // Eviction deliberately lets the producer overwrite a slot a doomed consumer
  // may still be copying (that is how slots reclaim immediately). The overlap
  // is unavoidable, so the payload is copied word-wise through relaxed atomics:
  // each word tears atomically instead of forming a plain-memory data race, the
  // access stays well-defined (and TSan-clean) rather than UB, and the doomed
  // batch is discarded by the post-copy epoch check anyway. Steady-state
  // ordering is unchanged -- these relaxed accesses still sit inside the
  // happens-before established by the m_produced release/acquire and the gate.
  struct alignas(k_cache_line) Slot {
    static constexpr std::size_t k_words{(sizeof(T) + (8 - 1)) / 8};
    std::array<std::atomic<std::uint64_t>, k_words> words{};

    void Store(const T& value) noexcept {
      std::array<std::uint64_t, k_words> raw{};
      std::memcpy(raw.data(), &value, sizeof(T));
      for (auto w{0UZ}; w < k_words; ++w) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
        words[w].store(raw[w], std::memory_order::relaxed);
      }
    }

    [[nodiscard]] T Load() const noexcept {
      std::array<std::uint64_t, k_words> raw{};
      for (auto w{0UZ}; w < k_words; ++w) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
        raw[w] = words[w].load(std::memory_order::relaxed);
      }
      T value{};
      std::memcpy(&value, raw.data(), sizeof(T));
      return value;
    }
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
  [[nodiscard]] std::atomic<Epoch>& EpochAt(std::size_t idx) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
    return m_cursors[idx].epoch;
  }
  [[nodiscard]] const std::atomic<Epoch>& EpochAt(
      std::size_t idx) const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
    return m_cursors[idx].epoch;
  }

  // Producer-write / consumer-read boundary; alone on its own line
  alignas(k_cache_line) std::atomic<SeqNum> m_produced{0};

  alignas(k_cache_line) SeqNum m_gate_cache{0};
  std::size_t m_consumer_count{0};

  // Off the hot path: only ever touched when a consumer is killed,
  // or when one detects that it was overtaken. Shared line, both are rare
  alignas(k_cache_line) std::atomic<std::uint64_t> m_evictions{0};
  std::atomic<std::uint64_t> m_lapped{0};

  // One padded cursor per consumer so no false sharing between readers or with
  // the producer's gate scan
  alignas(k_cache_line) std::array<CursorCell, MAX_CONSUMERS> m_cursors{};

  // Padded slots so a producer store to slot k never shares a line with a
  // consumer load from slot k-1. Storing to slot k dirties the entire cache
  // line meaning consumers must reload the cache just to read unchanged bytes
  //
  // Every slot line is pulled into all N reader caches so not good so up to N
  // dirtied caches every write
  alignas(k_cache_line) std::array<Slot, CAPACITY> m_buffer{};
};

}  // namespace lfob

#endif  // SPMC_RING_HPP_
