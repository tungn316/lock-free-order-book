#ifndef CLIENT_SESSION_HPP_
#define CLIENT_SESSION_HPP_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "gateway_config.hpp"
#include "outbound_buffer.hpp"
#include "types.hpp"

namespace lfob {

// <---- Client session ---->

// One connected client, touched by two threads: ClientIo reads orders off the
// socket, the owning OutputWorker writes reports to it. They share the object
// but not its fields -- the inbound parser never touches m_out, the worker
// never reads the socket -- so the only crossing point is the state, which is
// atomic.
//
// Lifetime is deliberately one-way: a session is created by ClientIo, marked
// CLOSED by whichever side notices the peer is gone, and destroyed only after
// every worker thread has joined. Nothing frees a session while a worker
// might still be holding a pointer to it
class ClientSession {
 public:
  enum class State : std::uint8_t {
    ACTIVE,
    GAPPED,   // dropped public data, owes a gap notice + snapshot
    CLOSING,  // private data was dropped, or the peer went away
    CLOSED,
  };

  ClientSession(ClientId id, int fd) noexcept;
  ~ClientSession();

  ClientSession(const ClientSession&) = delete;
  ClientSession(ClientSession&&) = delete;
  ClientSession& operator=(const ClientSession&) = delete;
  ClientSession& operator=(ClientSession&&) = delete;

  // --- Lossy path ---
  //
  // A full buffer marks the session GAPPED and drops the frame:
  // - market data is a stream of the present
  // - a client that is behind wants the current book
  bool PublishPublic(const std::byte* frame, std::size_t bytes) noexcept;

  // --- Lossless-or-disconnect path ---
  //
  // This client's own fills and acks cannot be dropped:
  // - a trader that never learns it was filled is worse off than one
  // that gets disconnected and reconciles by drop copy
  // - a full buffer moves the session to CLOSING instead
  bool PublishPrivate(const std::byte* frame, std::size_t bytes) noexcept;

  std::size_t Flush() noexcept;

  // --- Gap recovery by the owning worker ---
  // - announce the discontinuity
  // - then re-prime the client with a snapshot so it can resume from a known state
  //  Only after both land does the session go back to ACTIVE
  [[nodiscard]] bool NeedsResync() const noexcept;
  bool SendGapNotice(SeqNum resume_seq) noexcept;
  bool SendSnapshot(const Bbo& bbo) noexcept;
  void MarkGapped() noexcept;

  // --- Inbound read side (ClientIo thread only) ---
  //
  // The mirror of the worker's publish/flush side: only the ClientIo thread
  // ever touches the inbound buffer, so like m_out it needs no synchronization
  enum class FillResult : std::uint8_t {
    GOT_DATA,     // read some bytes; there may be more on the socket
    WOULD_BLOCK,  // nothing right now (EAGAIN), or the buffer is momentarily full
    CLOSED,       // peer hung up or the socket errored -- caller should Close()
  };
  enum class ReadResult : std::uint8_t {
    ORDER,      // `out` holds a decoded, validated command
    NONE,       // fewer than one whole record is buffered
    MALFORMED,  // a full record was buffered but failed validation
  };

  // Read whatever the socket has into the inbound staging buffer
  [[nodiscard]] FillResult FillInbound() noexcept;
  // Decode the next buffered WireOrder into `out`, stamped with this client id
  [[nodiscard]] ReadResult NextOrder(OrderCommand& out) noexcept;

  void Close() noexcept;

  [[nodiscard]] std::uint64_t Dropped() const noexcept;

  [[nodiscard]] ClientId Id() const noexcept;
  [[nodiscard]] int Fd() const noexcept;
  [[nodiscard]] State GetState() const noexcept;

 private:
  ClientId m_id;
  int m_fd;
  std::atomic<State> m_state{State::ACTIVE};

  // ClientIo read thread only below this line. Bytes land in [m_in_head,
  // m_in_tail); the prefix before m_in_head has been parsed and is reclaimed on
  // the next fill
  std::array<std::byte, k_session_inbound_bytes> m_in{};
  std::size_t m_in_head{0};
  std::size_t m_in_tail{0};

  // Worker thread only below this line
  SeqNum m_last_seq{0};  // last report sequence this client saw
  std::uint64_t m_dropped{0};
  OutboundBuffer m_out;
};

}  // namespace lfob

#endif  // CLIENT_SESSION_HPP_
