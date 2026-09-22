#include <lfob/client_session.hpp>

#include <lfob/execution_report.hpp>
#include <lfob/order_wire.hpp>
#include <lfob/report_wire.hpp>

#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace lfob {

ClientSession::ClientSession(ClientId id, int fd) noexcept
    : m_id(id), m_fd(fd) {}

ClientSession::~ClientSession() {
  if (m_fd >= 0) {
    ::close(m_fd);
  }
}

bool ClientSession::PublishPublic(const std::byte* frame,
                                  std::size_t bytes) noexcept {
  if (m_state.load(std::memory_order::relaxed) != State::ACTIVE) {
    return false;
  }

  if (m_out.TryAppend(frame, bytes)) {
    return true;
  }

  // Buffer full, drop frame and it GAPPED
  ++m_dropped;
  m_state.store(State::GAPPED, std::memory_order::relaxed);
  return false;
}

bool ClientSession::PublishPrivate(const std::byte* frame,
                                   std::size_t bytes) noexcept {
  const State state{m_state.load(std::memory_order::relaxed)};
  if (state == State::CLOSED || state == State::CLOSING) {
    return false;
  }

  if (m_out.TryAppend(frame, bytes)) {
    return true;
  }

  m_state.store(State::CLOSING, std::memory_order::relaxed);
  return false;
}

std::size_t ClientSession::Flush() noexcept {
  return m_out.FlushTo(m_fd);
}

[[nodiscard]] bool ClientSession::NeedsResync() const noexcept {
  return (m_state.load(std::memory_order::relaxed) == State::GAPPED);
}

void ClientSession::Close() noexcept {
  m_state = State::CLOSED;
}

bool ClientSession::SendGapNotice(SeqNum resume_seq) noexcept {
  const ExecutionReport notice{.seq = resume_seq,
                               .type = ExecutionReport::Type::GAP_NOTICE,
                               .client = m_id};
  // Same shared encoder as SendSnapshot / the worker fan-out, so the gap
  // notice is framed identically to every other report on the stream
  std::array<std::byte, sizeof(ExecutionReport)> frame{};
  const std::size_t n{EncodeReport(notice, frame.data(), frame.size())};
  return n != 0 && m_out.TryAppend(frame.data(), n);
}

bool ClientSession::SendSnapshot(const Bbo& bbo) noexcept {
  const ExecutionReport snap{.seq = bbo.event_seq,
                             .type = ExecutionReport::Type::TOP_OF_BOOK,
                             .client = m_id,
                             .bid_price = bbo.bid_price,
                             .bid_quantity = bbo.bid_quantity,
                             .ask_price = bbo.ask_price,
                             .ask_quantity = bbo.ask_quantity};

  // Encode through the shared EncodeReport (local scratch -- m_frames is
  // the worker's, out of reach here) so this frame is laid out identically
  // to every other report. Never reinterpret the struct straight into the
  // buffer, or a future wire format would make the snapshot diverge from
  // the rest of the stream and break the client's parse
  std::array<std::byte, sizeof(ExecutionReport)> frame{};
  const std::size_t n{EncodeReport(snap, frame.data(), frame.size())};
  if (n == 0 || !m_out.TryAppend(frame.data(), n)) {
    return false;
  }

  State expected{State::GAPPED};
  m_state.compare_exchange_strong(expected, State::ACTIVE,
                                  std::memory_order::relaxed);

  return true;
}

void ClientSession::MarkGapped() noexcept {
  State expected{State::ACTIVE};
  m_state.compare_exchange_strong(expected, State::GAPPED,
                                  std::memory_order::relaxed);
}

[[nodiscard]] std::uint64_t ClientSession::Dropped() const noexcept {
  return m_dropped;
}
[[nodiscard]] ClientId ClientSession::Id() const noexcept {
  return m_id;
}
[[nodiscard]] int ClientSession::Fd() const noexcept {
  return m_fd;
}
[[nodiscard]] ClientSession::State ClientSession::GetState() const noexcept {
  return m_state.load(std::memory_order::relaxed);
}

ClientSession::FillResult ClientSession::FillInbound() noexcept {
  // Reclaim the already-parsed prefix so a read always has somewhere to
  // land: reset when empty, or slide the unparsed tail to the front when
  // the buffer end is reached
  if (m_in_head == m_in_tail) {
    m_in_head = 0;
    m_in_tail = 0;
  } else if (m_in_tail == m_in.size() && m_in_head > 0) {
    m_in_tail -= m_in_head;
    std::memmove(m_in.data(), m_in.data() + m_in_head, m_in_tail);
    m_in_head = 0;
  }
  if (m_in_tail == m_in.size()) {
    return FillResult::WOULD_BLOCK;  // full of unparsed bytes; drain first
  }

  const ssize_t r{
      ::read(m_fd, m_in.data() + m_in_tail, m_in.size() - m_in_tail)};
  if (r > 0) {
    m_in_tail += static_cast<std::size_t>(r);
    return FillResult::GOT_DATA;
  }
  if (r == 0) {
    return FillResult::CLOSED;  // orderly peer shutdown (EOF)
  }
  // r < 0: nothing available now is normal; any other errno is a broken
  // socket, which we treat as a close
  return (errno == EAGAIN || errno == EWOULDBLOCK) ? FillResult::WOULD_BLOCK
                                                   : FillResult::CLOSED;
}

ClientSession::ReadResult ClientSession::NextOrder(OrderCommand& out) noexcept {
  if (m_in_tail - m_in_head < sizeof(WireOrder)) {
    return ReadResult::NONE;
  }

  WireOrder wire{};
  std::memcpy(&wire, m_in.data() + m_in_head, sizeof(wire));
  m_in_head += sizeof(WireOrder);

  // Stamp the command with THIS session's id -- a client can never set the
  // client field off the wire, so it cannot impersonate another
  return DecodeOrder(wire, m_id, out) ? ReadResult::ORDER
                                      : ReadResult::MALFORMED;
}

}  // namespace lfob
