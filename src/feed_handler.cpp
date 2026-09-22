#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // recvmmsg / struct mmsghdr are GNU extensions
#endif

#include <lfob/feed_handler.hpp>

#include <lfob/cpu_relax.h>
#include <lfob/affinity.hpp>
#include <lfob/gateway_config.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstddef>

namespace lfob {

FeedHandler::FeedHandler(MatchingEngine& engine, const Config& config) noexcept
    : m_engine(engine),
      m_config(config),
      m_translator(config.translate),
      m_mold(*this)  // this FeedHandler is the ItchSink MoldSession drives
{}

FeedHandler::~FeedHandler() {
  Stop();
  if (m_fd >= 0) {
    ::close(m_fd);
  }
}

bool FeedHandler::Join() noexcept {
  // Standing up a multicast receiver is a little ritual -- any step can
  // fail, and if one does we close the fd and bail so we never leak it:
  //
  //   1. socket()                 UDP, non-blocking
  //   2. SO_REUSEADDR             so several receivers can share the port
  //   3. bind()                   to the port on any local interface
  //   4. IP_ADD_MEMBERSHIP        actually subscribe to the group
  //
  // Non-blocking matters because Run() busy-polls: a quiet feed must never
  // park the thread inside recvmmsg
  m_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  if (m_fd < 0) {
    return false;
  }

  // Multiple receivers may bind the same multicast port; also lets us
  // rebind promptly on restart
  const int one{1};
  if (::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    ::close(m_fd);
    m_fd = -1;
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);  // receive on any interface
  addr.sin_port = htons(m_config.port);
  if (::bind(m_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) <
      0) {
    ::close(m_fd);
    m_fd = -1;
    return false;
  }

  // Join the multicast group. An empty iface means "the default", i.e.
  // let the kernel pick via INADDR_ANY
  ip_mreq mreq{};
  if (::inet_pton(AF_INET, m_config.group.data(), &mreq.imr_multiaddr) != 1) {
    ::close(m_fd);
    m_fd = -1;
    return false;
  }
  if (m_config.iface.at(0) == '\0') {
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  } else if (::inet_pton(AF_INET, m_config.iface.data(), &mreq.imr_interface) !=
             1) {
    ::close(m_fd);
    m_fd = -1;
    return false;
  }
  if (::setsockopt(m_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) <
      0) {
    ::close(m_fd);
    m_fd = -1;
    return false;
  }

  return true;
}

void FeedHandler::Start() {
  m_thread = std::jthread([this](const std::stop_token& stop) {
    ConfigureThread();
    Run(stop);
  });
}

void FeedHandler::Stop() noexcept {
  // Non-blocking recvmmsg returns promptly, so the loop observes the stop
  // token each pass -- no need to interrupt a blocked syscall
  if (m_thread.joinable()) {
    m_thread.request_stop();
    m_thread.join();
  }
}

void FeedHandler::ConfigureThread() const noexcept {
  // Own core, highest RT priority in the front end: the feed arrives on
  // someone else's schedule and starving it looks like market movement
  PinCurrentThread(m_config.cpu_core);
  SetRealtimePriority(k_feed_rt_priority);
}

void FeedHandler::Run(const std::stop_token& stop) {
  while (!stop.stop_requested()) {
    const std::size_t n{Receive()};
    FlushStaged();  // push whatever the batch decoded into commands
    if (n == 0) {
      CpuRelax();  // idle feed; hint the core we are spinning
    }
  }
  FlushStaged();  // drain anything staged at shutdown
}

std::size_t FeedHandler::Receive() noexcept {
  // recvmmsg grabs a whole batch of datagrams in ONE syscall (that is the
  // "mm" -- multi-message). It needs a little scaffolding: an mmsghdr per
  // datagram, each pointing at an iovec, each pointing at one of our
  // receive buffers. After the call, msgs[i].msg_len says how many bytes
  // datagram i actually was.
  //
  //   msgs[i].msg_hdr.msg_iov ─► iovs[i] ─► m_datagrams[i]  (up to 1472 B)
  //   msgs[i].msg_len          = bytes received into that buffer
  std::array<mmsghdr, k_datagram_batch> msgs{};
  std::array<iovec, k_datagram_batch> iovs{};
  for (std::size_t i{0}; i < k_datagram_batch; ++i) {
    iovs.at(i).iov_base = m_datagrams.at(i).data();
    iovs.at(i).iov_len = m_datagrams.at(i).size();
    msgs.at(i).msg_hdr.msg_iov = &iovs.at(i);
    msgs.at(i).msg_hdr.msg_iovlen = 1;
  }

  const int n{::recvmmsg(m_fd, msgs.data(),
                         static_cast<unsigned>(k_datagram_batch), 0, nullptr)};
  if (n <= 0) {
    return 0;  // EAGAIN (nothing waiting) or error; both are "idle" here
  }

  // Frame each datagram through MoldSession, which calls back into
  // OnMessage / OnGap on this same thread
  const auto received{static_cast<std::size_t>(n)};
  for (std::size_t i{0}; i < received; ++i) {
    m_mold.OnDatagram(m_datagrams.at(i).data(), msgs.at(i).msg_len);
  }
  return received;
}

void FeedHandler::OnMessage(const ItchMessage& msg) noexcept {
  // A prior gap left the rebuilt book divergent from the real market, so
  // stop trusting the stream until a recovery source re-anchors it
  if (m_stalled) {
    return;
  }

  OrderCommand cmd{};
  if (m_translator.Translate(msg, cmd) == ItchTranslator::Result::COMMAND) {
    m_staging.at(m_staged++) = cmd;
    if (m_staged == k_stage_batch) {
      FlushStaged();
    }
  }
  // Non-COMMAND results (filtered / halted / off-tick / out-of-band /
  // unsupported) carry no order; the translator counts its own rejects
}

void FeedHandler::OnGap(SeqNum expected, SeqNum received) noexcept {
  // No live-stream repair exists: stop submitting and wait for a recovery
  // source. MoldSession counts the gap; we just record that we are unwell
  (void)expected;
  (void)received;
  m_stalled = true;
}

void FeedHandler::FlushStaged() noexcept {
  if (m_staged == 0) {
    return;
  }
  const std::size_t accepted{m_engine.SubmitBulk(m_staging.data(), m_staged)};
  m_submitted.fetch_add(accepted, std::memory_order::relaxed);
  if (accepted < m_staged) {
    // Engine ingress ring was full. Unlike a client, the feed has no one
    // to reject to -- the command is simply dropped and counted
    m_rejected.fetch_add(m_staged - accepted, std::memory_order::relaxed);
  }
  m_staged = 0;
}

std::uint64_t FeedHandler::Submitted() const noexcept {
  return m_submitted.load(std::memory_order::relaxed);
}

std::uint64_t FeedHandler::Rejected() const noexcept {
  return m_rejected.load(std::memory_order::relaxed);
}

std::uint64_t FeedHandler::Gaps() const noexcept {
  return m_mold.Gaps();
}

bool FeedHandler::Healthy() const noexcept {
  return m_fd >= 0 && !m_stalled;
}

}  // namespace lfob
