#include <lfob/affinity.hpp>

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace lfob {

// Pin the calling thread to a single core for the core matching engine
bool PinCurrentThread(int core) noexcept {
  if (core < 0) {
    return false;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(core), &set);
  // pthread_setaffinity_np returns an errno value (0 == success), it does
  // NOT set the global errno. Do not check errno here.
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

bool SetRealtimePriority(int priority) noexcept {
  const int lo = sched_get_priority_min(SCHED_FIFO);
  const int hi = sched_get_priority_max(SCHED_FIFO);
  if (lo < 0 || hi < 0 || priority < lo || priority > hi) {
    return false;
  }
  sched_param param{};
  param.sched_priority = priority;
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
}

bool LockMemory() noexcept {
  return mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
}

// Touch every page in [base, base + bytes) to force the kernel to allocate and
// map physical frames now
bool Prefault(void* base, std::size_t bytes) noexcept {
  if (base == nullptr || bytes == 0) {
    return false;
  }
  const long page = sysconf(_SC_PAGESIZE);
  if (page <= 0) {
    return false;
  }
  auto* p = static_cast<volatile std::uint8_t*>(base);
  const std::size_t step = static_cast<std::size_t>(page);
  for (std::size_t off{0}; off < bytes; off += step) {
    static_cast<void>(p[off]);
  }
  // Ensure the final partial page is touched even if bytes isn't page-aligned.
  static_cast<void>(p[bytes - 1]);
  return true;
}

}  // namespace lfob
