#ifndef AFFINITY_HPP_
#define AFFINITY_HPP_

#include <cstddef>

namespace lfob {

// Pin calling thread to single core (for single core matching engine)
bool PinCurrentThread(int core) noexcept;
bool SetRealtimePriority(int priority) noexcept;
bool LockMemory() noexcept;
bool Prefault(void* base, std::size_t bytes) noexcept;

}  // namespace lfob

#endif // AFFINITY_HPP_
