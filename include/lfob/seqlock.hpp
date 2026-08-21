#ifndef SEQLOCK_HPP_
#define SEQLOCK_HPP_

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include "cpu_relax.h"
#include "types.hpp"

// Single Writer Multiple Reader Seqlock
namespace lfob {

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

  // The fence ensures that we increment m_seq to odd before we start writing to m_value
  // The store release promises if we acquire we have fully written to m_value
  void Store(const T& value) noexcept {
    const SeqNum seq{m_seq.load(std::memory_order::relaxed)};
    m_seq.store(seq + 1, std::memory_order::relaxed);

    std::atomic_thread_fence(std::memory_order::release);

    CopyTo(&value);
    m_seq.store(seq + 2, std::memory_order::release);
  }

  // The acquire load pairs with the writer's release store:
  // - Observe an even seq, the promise is for this seq the CopyTo has fully completed before our CopyFrom
  // - Nothing stopping a CopyTo right the second we accept an even seq, that is the reason for (seq0 == seq1)
  // - The acquire fence ensures we don't load seq1 before we finish copying m_value
  [[nodiscard]] T Load() const noexcept {
    T value{};
    for (;;) {
      const SeqNum seq0{m_seq.load(std::memory_order::acquire)};
      if ((seq0 & 1U) != 0) {
        CpuRelax(); // hint to processor that we are in spin-lock
        continue;
      }
      CopyFrom(&value);
      std::atomic_thread_fence(std::memory_order::acquire);
      const SeqNum seq1{m_seq.load(std::memory_order::relaxed)};
      if (seq0 == seq1) {
        return value;
      }
      CpuRelax(); // hint to processor that we are in spin-lock
    }
  }

 private:
  std::atomic<SeqNum> m_seq{0};  // odd = in progress

  // Payload stored as raw atomic bytes the concurrent byte-wise copy in
  // Store/Load is data-race-free under the C++ memory model. unsigned char
  // atomics are always lock-free, so this stays lock-free for any size T.
  alignas(T) std::array<std::atomic<unsigned char>, sizeof(T)> m_value{};

  // The atomics are here only to make each conflicting byte access well-defined
  void CopyTo(const T* src) noexcept {
    auto* v = m_value.data();
    const auto* s = reinterpret_cast<const unsigned char*>(src);
    for (std::size_t i{0}; i < sizeof(T); ++i) {
      v[i].store(s[i], std::memory_order::relaxed);
    }
  }
  void CopyFrom(T* dst) const noexcept {
    auto* v = m_value.data();
    auto* d = reinterpret_cast<unsigned char*>(dst);
    for (std::size_t i{0}; i < sizeof(T); ++i) {
      d[i] = v[i].load(std::memory_order::relaxed);
    }
  }
};

}  // namespace lfob

#endif  // SEQLOCK_HPP_
