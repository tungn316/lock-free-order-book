#ifndef AFFINITY_HPP_
#define AFFINITY_HPP_

#include <cstddef>

namespace lfob {

// Pin calling thread to single core (for single core matching engine)
bool PinCurrentThread(int core) noexcept;

// Switches thread to SCHED_FIFO real-time scheduling class, will run until it blocks or yields, won't get descheduled by a normal class thread
bool SetRealtimePriority(int priority) noexcept;

// Pin all current and future pages of the process into RAM -> no later page faults
bool LockMemory() noexcept;

// Touch every page we use so the kernal allocates a physical frame at startup
bool Prefault(void* base, std::size_t bytes) noexcept;

}  // namespace lfob

#endif  // AFFINITY_HPP_
