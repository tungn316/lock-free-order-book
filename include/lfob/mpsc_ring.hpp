#ifndef MPSC_QUEUE_HPP_
#define MPSC_QUEUE_HPP_

#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <array>
#include "types.hpp"

namespace lfob {

// Bounded lock-free MPSC ring.
template <typename T, std::size_t CAPACITY>
class MpscRing {
  static_assert((CAPACITY & (CAPACITY - 1)) == 0,
                "Capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  MpscRing() noexcept
  {
    for (auto i{0UZ}; i < CAPACITY; ++i) {
        m_buffer[i].ready.store(static_cast<SeqNum>(i), std::memory_order::relaxed);
    }
  }
  ~MpscRing() noexcept = default;
  MpscRing(const MpscRing&) = delete;
  MpscRing(MpscRing&&) = delete;
  MpscRing& operator=(const MpscRing&) = delete;
  MpscRing& operator=(MpscRing&&) = delete;

  bool TryPush(const T& item) noexcept
  {
      std::size_t tail = m_tail.load(std::memory_order::relaxed);

      for (;;) {
          Slot& slot = m_buffer[tail & (CAPACITY - 1)];
          const SeqNum ready = slot.ready.load(std::memory_order::acquire);
          const SeqNum expected = static_cast<SeqNum>(tail);
          const auto dif = static_cast<std::make_signed_t<SeqNum>>(ready - expected);

          // ready == tail means it is ready to be written to
          if (dif == 0) {
              if (m_tail.compare_exchange_weak(tail, tail + 1, std::memory_order::relaxed, std::memory_order::relaxed)) {
                  slot.payload = item;
                  slot.ready.store(expected + 1, std::memory_order::release);
                  return true;
              }
          }
          // tail has hit a ready that is at least CAPACITY away from head and since no pop has occured
          // ready hasn't incremented by CAPACITY to be included in the next MOD
          else if (dif < 0) {
              return false;
          }
          // stale tail, reload and try again
          else {
              tail = m_tail.load(std::memory_order::relaxed);
          }
      }
  }
  std::size_t TryPushBulk(const T* items, std::size_t count) noexcept
  {
      if (count == 0) { return 0; }
      std::size_t tail = m_tail.load(std::memory_order::relaxed);

      for (;;) {
          std::size_t n{0};
          while (n < count) {
              Slot& slot = m_buffer[(tail + n) & (CAPACITY - 1)];
              const SeqNum ready = slot.ready.load(std::memory_order::acquire);
              const auto dif = static_cast<std::make_signed_t<SeqNum>>(ready - static_cast<SeqNum>(tail + n));
              if (dif != 0) { break; }
              ++n;
          }
          if (n == 0) { return 0; }

          if (m_tail.compare_exchange_weak(tail, tail + n, std::memory_order::relaxed, std::memory_order::relaxed)) {
              for (auto i{0UZ}; i < n; ++i) {
                  Slot& slot = m_buffer[(tail + i) & (CAPACITY - 1)];
                  const SeqNum expected = static_cast<SeqNum>(tail + i);
                  slot.payload = items[i];
                  slot.ready.store(expected + 1, std::memory_order::release);
              }
              return n;
          } else {
              continue;
          }

      }

  }

  std::optional<T> TryPop() noexcept
  {
      Slot& slot = m_buffer[m_head & (CAPACITY - 1)];
      const SeqNum ready = slot.ready.load(std::memory_order::acquire);
      const SeqNum expected = static_cast<SeqNum>(m_head) + 1;

      // slot.ready = m_head + 1 means it has been written to
      if (ready == expected) {
          T value = slot.payload;
          slot.ready.store(static_cast<SeqNum>(m_head) + CAPACITY, std::memory_order::release);
          ++m_head;
          return value;
      }

      else {
          return std::nullopt;
      }
  }
  std::size_t TryPopBulk(T* out, std::size_t max) noexcept
  {
      std::size_t n{0};
      for (auto i{0UZ}; i < max; ++i) {
          Slot& slot = m_buffer[m_head & (CAPACITY - 1)];
          const SeqNum ready = slot.ready.load(std::memory_order::acquire);
          const SeqNum expected = static_cast<SeqNum>(m_head) + 1;

          // slot.ready = m_head + 1 means it has been written to
          if (ready == expected) {
              ++n;
              slot.ready.store(static_cast<SeqNum>(m_head) + CAPACITY, std::memory_order::release);
              out[i] = slot.payload;
              ++m_head;
          } else {
              break;
          }
      }
      return n;
  }

  // Approximate deapth, only used by consumer!
  [[nodiscard]] std::size_t SizeApprox() const noexcept
  {
      return (m_tail.load(std::memory_order::relaxed) - m_head);
  }

 private:
  struct alignas(k_cache_line) Slot {
    std::atomic<SeqNum> ready;
    T payload;
  };

  alignas(k_cache_line) std::atomic<std::size_t> m_tail{0};
  alignas(k_cache_line) std::size_t m_head{0};
  alignas(k_cache_line) std::array<Slot, CAPACITY> m_buffer;
};

}  // namespace lfob

#endif // MPSC_QUEUE_HPP_
