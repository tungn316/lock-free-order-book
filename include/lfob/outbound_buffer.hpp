#ifndef OUTBOUND_BUFFER_HPP_
#define OUTBOUND_BUFFER_HPP_

#include <array>
#include <cstddef>

namespace lfob {

// Per-session outbound staging. Sized for roughly a second of quotes at a
// modest rate: past this the client is not lagging, it is gone
inline constexpr std::size_t k_session_outbound_bytes{1U << 18U};  // 256 KiB

// Bounded single-producer byte ring drained by non-blocking writev. Owned by
// exactly one OutputWorker thread, so it needs no atomics: the "single
// producer" is the worker, and the socket is the only consumer
//
// matching thread ──► SpmcRing ──► OutputWorker ──► OutboundBuffer ──► client
// socket
//                     └─ 2 ms budget ─┘             └─ 1 s / 256 KiB ─┘
//                     "worker isn't reading"        "client isn't reading"
//
class OutboundBuffer {
 public:
  OutboundBuffer() = default;
  ~OutboundBuffer() = default;
  OutboundBuffer(const OutboundBuffer&) = delete;
  OutboundBuffer(OutboundBuffer&&) = delete;
  OutboundBuffer& operator=(const OutboundBuffer&) = delete;
  OutboundBuffer& operator=(OutboundBuffer&&) = delete;

  // All-or-nothing: a frame is never half-written, so the stream stays
  // parseable no matter where the drop lands
  bool TryAppend(const std::byte* frame, std::size_t bytes) noexcept;

  // One writev of whatever is contiguous. Returns bytes handed to the kernel;
  // 0 on EAGAIN, which is the normal "socket is full" answer and not an error
  std::size_t FlushTo(int fd) noexcept;

  [[nodiscard]] std::size_t Pending() const noexcept;
  [[nodiscard]] std::size_t Free() const noexcept;
  [[nodiscard]] bool Empty() const noexcept;
  void Clear() noexcept;

 private:
  std::array<std::byte, k_session_outbound_bytes> m_bytes{};
  std::size_t m_head{0};
  std::size_t m_tail{0};
};

}  // namespace lfob

#endif  // OUTBOUND_BUFFER_HPP_
