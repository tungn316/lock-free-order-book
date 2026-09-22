#ifndef SEQLOCK_HPP_
#define SEQLOCK_HPP_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "cpu_relax.h"
#include "types.hpp"

// Single Writer Multiple Reader Seqlock
namespace lfob {

template <typename T>
class alignas(k_cache_line) Seqlock {
  static_assert(std::is_trivially_copyable_v<T>);
  // The payload is copied word-wise through lock-free 64-bit atomics (see the
  // note on m_value); a target without those would silently take a mutex.
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

 public:
  Seqlock() = default;
  Seqlock(const Seqlock&) = delete;
  Seqlock(Seqlock&&) = delete;
  Seqlock& operator=(const Seqlock&) = delete;
  Seqlock& operator=(Seqlock&&) = delete;
  ~Seqlock() = default;

  // The fence ensures that we increment m_seq to odd before we start writing to
  // m_value The store release promises if we acquire we have fully written to
  // m_value
  void Store(const T& value) noexcept {
    const SeqNum seq{m_seq.load(std::memory_order::relaxed)};
    m_seq.store(seq + 1, std::memory_order::relaxed);

    std::atomic_thread_fence(std::memory_order::release);

    CopyTo(&value);
    m_seq.store(seq + 2, std::memory_order::release);
  }

  // The acquire load pairs with the writer's release store:
  // - Observe an even seq, the promise is for this seq the CopyTo has fully
  // completed before our CopyFrom
  // - Nothing stopping a CopyTo right the second we accept an even seq, that is
  // the reason for (seq0 == seq1)
  // - The acquire fence ensures we don't load seq1 before we finish copying
  // m_value
  [[nodiscard]] T Load() const noexcept {
    T value{};
    for (;;) {
      const SeqNum seq0{m_seq.load(std::memory_order::acquire)};
      if ((seq0 & 1U) != 0) {
        CpuRelax();  // hint to processor that we are in spin-lock
        continue;
      }
      CopyFrom(&value);
      std::atomic_thread_fence(std::memory_order::acquire);
      const SeqNum seq1{m_seq.load(std::memory_order::relaxed)};
      if (seq0 == seq1) {
        return value;
      }
      CpuRelax();  // hint to processor that we are in spin-lock
    }
  }

 private:
  std::atomic<SeqNum> m_seq{0};  // odd = in progress

  // Payload copied word-wise through relaxed atomics, matching SpmcRing::Slot:
  // each word access is well-defined under the concurrent Store/Load race, and
  // the seq re-check discards any torn read. The atomics exist only to remove
  // the data-race UB; the seq counter is what keeps the whole T consistent.
  //
  // An 8-byte atomic can't legally view T's bytes directly (strict aliasing)
  // and sizeof(T) need not be a multiple of 8, so a plain uint64 array bridges
  // the two via memcpy -- alias-safe, and zero-padding the ragged tail. Fewer
  // atomic ops than a byte-wise copy (sizeof(T)/8 vs sizeof(T)).
  static constexpr std::size_t k_words{(sizeof(T) + sizeof(std::uint64_t) - 1) /
                                       sizeof(std::uint64_t)};
  alignas(T) std::array<std::atomic<std::uint64_t>, k_words> m_value{};

  void CopyTo(const T* src) noexcept {
    std::array<std::uint64_t, k_words> raw{};
    std::memcpy(raw.data(), src, sizeof(T));
    for (auto w{0UZ}; w < k_words; ++w) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
      m_value[w].store(raw[w], std::memory_order::relaxed);
    }
  }
  void CopyFrom(T* dst) const noexcept {
    std::array<std::uint64_t, k_words> raw{};
    for (auto w{0UZ}; w < k_words; ++w) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
      raw[w] = m_value[w].load(std::memory_order::relaxed);
    }
    std::memcpy(dst, raw.data(), sizeof(T));
  }
};

}  // namespace lfob

#endif  // SEQLOCK_HPP_
