#include <lfob/outbound_buffer.hpp>

#include <cstring>
#include <sys/uio.h>

namespace lfob {

bool OutboundBuffer::TryAppend(const std::byte* frame, std::size_t bytes) noexcept {
  if (bytes > Free()) {
    return false;
  }
  constexpr std::size_t k_mask{k_session_outbound_bytes - 1};
  const std::size_t offset{m_tail & k_mask};

  // Split the copy at the array boundary when the frame straddles the end
  if (offset + bytes > k_session_outbound_bytes) {
    const std::size_t first{k_session_outbound_bytes - offset};
    std::memcpy(&m_bytes.at(offset), frame, first);
    std::memcpy(&m_bytes.at(0), frame + first, bytes - first);
  } else {
    std::memcpy(&m_bytes.at(offset), frame, bytes);
  }

  m_tail += bytes;
  return true;
}

std::size_t OutboundBuffer::FlushTo(int fd) noexcept {
  const std::size_t pending{Pending()};
  if (pending == 0) {
    return 0;
  }

  constexpr std::size_t k_mask{k_session_outbound_bytes - 1};
  const std::size_t offset{m_head & k_mask};

  // The readable region is one span, or two if it wraps the end of the array.
  // Describe it as up to two iovecs and let a single writev drain both.
  std::array<::iovec, 2> iov{};
  int iovcnt{0};

  const std::size_t contiguous{k_session_outbound_bytes - offset};
  const std::size_t first{pending < contiguous ? pending : contiguous};
  iov.at(iovcnt++) = {.iov_base = &m_bytes.at(offset), .iov_len = first};
  if (first < pending) {
    iov.at(iovcnt++) = {.iov_base = &m_bytes.at(0), .iov_len = pending - first};
  }

  const ssize_t n{::writev(fd, iov.data(), iovcnt)};
  if (n <= 0) {
    // EAGAIN/EWOULDBLOCK: send buffer full, retry next poll.
    // EINTR: interrupted before any byte moved, same.
    // EPIPE/ECONNRESET/...: dead socket; report 0 and let ClientIo's epoll
    //   (EPOLLHUP/EPOLLERR on the read side) drive the teardown.
    return 0;
  }

  m_head += static_cast<std::size_t>(n);
  return static_cast<std::size_t>(n);
}

std::size_t OutboundBuffer::Pending() const noexcept { return m_tail - m_head; }

std::size_t OutboundBuffer::Free() const noexcept {
  return k_session_outbound_bytes - (m_tail - m_head);
}

bool OutboundBuffer::Empty() const noexcept { return m_tail == m_head; }

void OutboundBuffer::Clear() noexcept { m_head = m_tail; }

}  // namespace lfob
