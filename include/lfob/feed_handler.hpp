#ifndef FEED_HANDLER_HPP_
#define FEED_HANDLER_HPP_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <thread>

#include "itch.hpp"
#include "matching_engine.hpp"
#include "types.hpp"

namespace lfob {

// <---- Ingress: ITCH feed ---->

// The other way orders get into the engine (the first being client TCP via
// ClientIo). This one pulls NASDAQ's MoldUDP64 multicast, runs it through the
// itch.hpp stack, and submits whatever commands come out. It gets its own
// thread on its own core, because the feed arrives on the market's schedule,
// not ours -- if we let it queue behind client I/O, the lag would look exactly
// like the market went quiet.
//
// We ARE the ItchSink: MoldSession calls our OnMessage / OnGap back on this
// same thread. So the whole loop lives inside one thread, no locks:
//
//   Run  (busy-poll on our core)
//    │
//    ├─ Receive()      recvmmsg a batch of up to 32 datagrams in one syscall
//    │     │
//    │     └─ for each: MoldSession::OnDatagram(...)
//    │                        │  (frames + de-dups, then calls us back)
//    │                        ▼
//    │                   OnMessage(msg) ─► ItchTranslator ─► stage OrderCommand
//    │                   OnGap(...)      ─► m_stalled = true (stop trusting feed)
//    │
//    └─ FlushStaged()  SubmitBulk the staged commands to the engine's ingress
//
// A gap is the scary case: UDP dropped a run of messages that the live feed
// will never resend, so the book we are rebuilding is now wrong. We can't
// invent the missing orders, so OnGap just stops submitting and flags us
// unhealthy until a recovery source re-anchors the stream.
class FeedHandler final : public ItchSink {
 public:
  struct Config {
    int cpu_core;
    std::uint16_t port;
    std::array<char, 64> group;  // multicast group, NUL-terminated
    std::array<char, 64> iface;  // local interface to join on
    ItchTranslator::Config translate;
  };

  FeedHandler(MatchingEngine& engine, const Config& config) noexcept;
  ~FeedHandler() override;

  FeedHandler(const FeedHandler&) = delete;
  FeedHandler(FeedHandler&&) = delete;
  FeedHandler& operator=(const FeedHandler&) = delete;
  FeedHandler& operator=(FeedHandler&&) = delete;

  bool Join() noexcept;  // open the socket, join the group
  void Start();
  void Stop() noexcept;

  // ItchSink. Both run on the feed thread, called from MoldSession
  void OnMessage(const ItchMessage& msg) noexcept override;

  // Called when MoldSession spots a gap. We can't repair it from the live
  // stream (the missing orders are gone for good), and pushing on through would
  // fill the book with orders the real market has already removed -- so we just
  // stop submitting and wait for a recovery source to re-anchor us
  void OnGap(SeqNum expected, SeqNum received) noexcept override;

  [[nodiscard]] std::uint64_t Submitted() const noexcept;
  [[nodiscard]] std::uint64_t Rejected() const noexcept;
  [[nodiscard]] std::uint64_t Gaps() const noexcept;
  [[nodiscard]] bool Healthy() const noexcept;

 private:
  void ConfigureThread() const noexcept;
  void Run(const std::stop_token& stop);

  // recvmmsg into the datagram buffers, then MoldSession::OnDatagram on each
  std::size_t Receive() noexcept;
  void FlushStaged() noexcept;

  static constexpr std::size_t k_datagram_batch{32};

  MatchingEngine& m_engine;
  Config m_config;
  ItchTranslator m_translator;
  MoldSession m_mold;

  int m_fd{-1};
  bool m_stalled{false};  // OnGap sets it; stays set until recovery (not wired yet)

  // One receive buffer per datagram in a batch, so recvmmsg can drop a whole
  // burst into m_datagrams in a single syscall
  std::array<std::array<std::byte, k_mold_max_datagram>, k_datagram_batch>
      m_datagrams{};
  std::array<OrderCommand, k_stage_batch> m_staging{};  // commands waiting on a flush
  std::size_t m_staged{0};
  std::jthread m_thread;

  std::atomic<std::uint64_t> m_submitted{0};
  std::atomic<std::uint64_t> m_rejected{0};
};

}  // namespace lfob

#endif  // FEED_HANDLER_HPP_
