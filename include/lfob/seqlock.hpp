#ifndef SEQLOCK_HPP_
#define SEQLOCK_HPP_

#include <atomic>
#include <cstring>
#include <type_traits>
#include "cpu_relax.h"
#include "types.hpp"

namespace lfob {

struct Bbo {
  SeqNum event_seq;
  Price bid_price;
  Quantity bid_qty;
  Price ask_price;
  Quantity ask_qty;
};

template <typename T>
class alignas(k_cache_line) Seqlock {
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  Seqlock() = default;
  Seqlock(const Seqlock&) = delete;
  Seqlock(Seqlock&&) = delete;
  Seqlock& operator=(const Seqlock&) = delete;
  Seqlock& operator=(Seqlock&&) = delete;
  ~Seqlock() = default;

  // single writer
  void Store(const T& value) noexcept {
    const SeqNum seq = m_seq.load(std::memory_order::relaxed);
    m_seq.store(seq + 1, std::memory_order::relaxed);
    std::atomic_thread_fence(std::memory_order::release);
    CopyTo(&m_value, &value);
    m_seq.store(seq + 2, std::memory_order::release);
  }
  // readers retry on torn read
  [[nodiscard]] T Load() const noexcept {
    T value{};
    for (;;) {
      const SeqNum seq0 = m_seq.load(std::memory_order::acquire);
      if ((seq0 & 1U) != 0) {
        CpuRelax();
        continue;
      }
      CopyFrom(&value, &m_value);
      std::atomic_thread_fence(
          std::memory_order::acquire);  // all preceding loads must complete
                                        // before this line
      const SeqNum seq1 = m_seq.load(std::memory_order::relaxed);
      if (seq0 == seq1) {
        return value;
      }
      CpuRelax();
    }
  }

 private:
  std::atomic<SeqNum> m_seq{0};  // odd = in progress

  // Byte-wise volatile write and read
  // Use instead of memcpy so compiler doesn't do any funny business
  static void CopyTo(T* dst, const T* src) noexcept {
    auto* d = reinterpret_cast<volatile unsigned char*>(dst);
    const auto* s = reinterpret_cast<const unsigned char*>(src);
    for (std::size_t i{0}; i < sizeof(T); ++i) {
      d[i] = s[i];
    }
  }
  static void CopyFrom(T* dst, const T* src) noexcept {
    auto* d = reinterpret_cast<unsigned char*>(dst);
    const auto* s = reinterpret_cast<const volatile unsigned char*>(src);
    for (std::size_t i{0}; i < sizeof(T); ++i) {
      d[i] = s[i];
    }
  }

  T m_value{};
};

}  // namespace lfob

#endif  // SEQLOCK_HPP_
