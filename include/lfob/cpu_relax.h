#ifndef CPU_RELAX_H_
#define CPU_RELAX_H_

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
static inline void CpuRelax() noexcept {
  _mm_pause();
}
#elifdef __aarch64__
#include <arm_acle.h>
static inline void CpuRelax() noexcept {
  __yield();
}
#elifdef __arm__
#include <arm_acle.h>
static inline void CpuRelax() noexcept {
  __yield();
}
#else
static inline void CpuRelax() noexcept {}
#endif

#endif  // CPU_RELAX_H_
