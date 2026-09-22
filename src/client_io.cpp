#include <lfob/client_io.hpp>

#include <lfob/affinity.hpp>
#include <lfob/execution_report.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <new>

namespace lfob {

ClientIo::ClientIo(MatchingEngine& engine, int cpu_core) noexcept
    : m_engine(engine), m_cpu_core(cpu_core) {
  m_sessions.reserve(k_max_client_sessions);
  m_workers.reserve(k_max_output_workers);
}

ClientIo::~ClientIo() {
  // Stop the accept/read thread first
  Stop();

  // Release the fds this object owns.
  if (m_epoll_fd >= 0) {
    ::close(m_epoll_fd);
  }
  if (m_listen_fd >= 0) {
    ::close(m_listen_fd);
  }
}

bool ClientIo::Listen(std::uint16_t port) noexcept {
  // TCP, non-blocking: a non-blocking accept() lets the thread poll many fds
  // without ever stalling on any one of them.
  m_listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (m_listen_fd < 0) {
    return false;
  }

  // Rebind immediately on restart instead of waiting out the kernel's
  // TIME_WAIT on the old socket (the classic "Address already in use").
  const int one{1};
  if (::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) <
      0) {
    ::close(m_listen_fd);
    m_listen_fd = -1;
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);  // listen on every local interface
  addr.sin_port = htons(port);               // host->network byte order
  if (::bind(m_listen_fd, reinterpret_cast<const sockaddr*>(&addr),
             sizeof(addr)) < 0) {
    ::close(m_listen_fd);
    m_listen_fd = -1;
    return false;
  }

  if (::listen(m_listen_fd, SOMAXCONN) <
      0) {  // start queuing incoming connections
    ::close(m_listen_fd);
    m_listen_fd = -1;
    return false;
  }

  // One epoll instance is the readiness list for the listener AND every client
  // socket OnAcceptable later adds.
  m_epoll_fd = ::epoll_create1(0);
  if (m_epoll_fd < 0) {
    ::close(m_listen_fd);
    m_listen_fd = -1;
    return false;
  }

  // Tag the listener with data.ptr == nullptr. Client sockets will be added
  // with data.ptr == the ClientSession*, so Run() tells "new connection" from
  // "client readable" by a null check -- no fd->session lookup needed.
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.ptr = nullptr;
  if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_listen_fd, &ev) < 0) {
    ::close(m_epoll_fd);
    ::close(m_listen_fd);
    m_epoll_fd = -1;
    m_listen_fd = -1;
    return false;
  }

  return true;
}

void ClientIo::Start() {
  assert(m_listen_fd >= 0 && m_epoll_fd >= 0 &&
         "Listen() must precede Start()");
  m_thread = std::jthread([this](const std::stop_token& stop) {
    ConfigureThread();  // pin core + set priority ON this thread
    Run(stop);
  });
}

void ClientIo::Stop() noexcept {
  // Idempotent: Gateway::Stop() calls this, then ~ClientIo calls it again.
  // No ring cursor to retire (unlike OutputWorker) -- just end the loop.
  if (m_thread.joinable()) {
    m_thread.request_stop();
    m_thread.join();
  }
}

void ClientIo::ConfigureThread() const noexcept {
  // Pin to the configured core, but deliberately NOT real-time: client
  // intake must stay below the matching thread and the output workers, so a
  // burst of connections can never preempt the engine or the fan-out
  PinCurrentThread(m_cpu_core);
}

void ClientIo::AddWorker(OutputWorker& worker) noexcept {
  m_workers.push_back(&worker);
}

std::uint64_t ClientIo::Accepted() const noexcept {
  return m_accepted.load(std::memory_order::relaxed);
}

std::uint64_t ClientIo::Rejected() const noexcept {
  return m_rejected.load(std::memory_order::relaxed);
}

void ClientIo::OnAcceptable() noexcept {
  // The listener fired. epoll can fold several pending connections into one
  // readiness signal, so drain until accept4() reports EAGAIN.
  while (true) {
    sockaddr_in peer{};
    socklen_t peer_len{sizeof(peer)};

    // accept4 + SOCK_NONBLOCK makes the NEW socket non-blocking in one
    // syscall -- the listener's flag does not propagate to accepted fds,
    // and a blocking client socket would let one slow reader stall Run
    const int cfd{::accept4(m_listen_fd, reinterpret_cast<sockaddr*>(&peer),
                            &peer_len, SOCK_NONBLOCK)};
    if (cfd < 0) {
      if (errno == EINTR) {
        continue;
      }       // interrupted, retry
      break;  // EAGAIN => queue drained
    }

    // Refuse if the table is full or there is nowhere to route egress.
    // Refusing is a normal outcome; growing unbounded is not
    if (m_sessions.size() >= k_max_client_sessions || m_workers.empty()) {
      ::close(cfd);
      m_rejected.fetch_add(1, std::memory_order::relaxed);
      continue;
    }

    // nothrow: this path is noexcept and a session is ~256 KiB (the
    // outbound buffer lives inline), so an allocation failure must
    // reject the client, never terminate the process
    const ClientId id{m_next_client++};
    std::unique_ptr<ClientSession> session(new (std::nothrow)
                                               ClientSession(id, cfd));
    if (!session) {
      ::close(cfd);
      m_rejected.fetch_add(1, std::memory_order::relaxed);
      continue;
    }

    // Register BEFORE the worker handoff. If epoll fails we unwind
    // cleanly here; if we enqueued first, a later failure would leave a
    // pointer to a destroyed session sitting in the worker's inbox
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = session.get();  // tag: Run dispatches straight to OnReadable
    if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
      m_rejected.fetch_add(1, std::memory_order::relaxed);
      continue;  // session unwinds -> ~ClientSession closes cfd (removes it
                 // from epoll)
    }

    // Hand egress to a worker, round-robin. Adopt enqueues onto the
    // worker's inbox; a full inbox means reject. The session is still
    // alive (owned by the local unique_ptr) when the worker may drain it
    OutputWorker* w{m_workers.at(m_next_worker)};
    m_next_worker = (m_next_worker + 1) % m_workers.size();
    if (!w->Adopt(session.get())) {
      ::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, cfd, nullptr);
      m_rejected.fetch_add(1, std::memory_order::relaxed);
      continue;  // session unwinds -> closes cfd
    }

    // ClientIo keeps ownership; moving the unique_ptr does not move the
    // ClientSession, so the raw pointer the worker just got stays valid
    m_sessions.push_back(std::move(session));
    m_accepted.fetch_add(1, std::memory_order::relaxed);
  }
}

std::size_t ClientIo::SubmitStaged() noexcept {
  if (m_staged == 0) {
    return 0;
  }

  // One batched push onto the engine's ingress ring. SubmitBulk stamps
  // each command's ingress_ts, accepts a contiguous prefix, and returns
  // how many it took; a shortfall means the ring is full
  const std::size_t accepted{m_engine.SubmitBulk(m_staging.data(), m_staged)};

  // The engine took cmds[0, accepted); everything from `accepted` on never
  // reached it, so it will never emit their rejects. We owe each of those
  // clients an INGRESS_FULL reject. (The engine already counted the
  // shortfall in its ingress-reject stat, so we do not count again here.)
  for (std::size_t i{accepted}; i < m_staged; ++i) {
    RejectForBackpressure(m_staging.at(i));
  }

  m_staged = 0;  // batch fully handled: every command accepted or rejected
  return accepted;
}

void ClientIo::RejectForBackpressure(const OrderCommand& cmd) noexcept {
  // The engine's ingress ring was full, so this command never reached the
  // book -- but the client still owes an answer. Build the reject it would
  // have gotten and inject it onto the egress via the engine's thread-safe
  // queue; the matching thread stamps the seq and publishes it, so a worker
  // delivers it exactly like any other report. seq/match_ts are left unset
  const ExecutionReport reject{
      .ingress_ts = cmd.ingress_ts,
      .type = ExecutionReport::Type::REJECTED,
      .side = cmd.side,
      .reason = ExecutionReport::RejectReason::INGRESS_FULL,
      .client = cmd.client,
      .order_id = cmd.id,
      .price = cmd.price,
  };

  // Best effort: if the inject queue is full too, the system is saturated
  // end to end and the client will time out this order. Nothing safe to do
  (void)m_engine.InjectReport(reject);
}

void ClientIo::OnReadable(ClientSession& session) noexcept {
  bool close_it{false};

  // Drain the socket: one readable notification can carry many orders, so
  // read and parse until the socket is empty (WOULD_BLOCK) or gone (CLOSED)
  for (;;) {
    const ClientSession::FillResult fill{session.FillInbound()};

    // Extract every complete order the buffer now holds into staging,
    // submitting a full batch as soon as one accumulates
    for (;;) {
      OrderCommand cmd{};
      const ClientSession::ReadResult res{session.NextOrder(cmd)};
      if (res == ClientSession::ReadResult::NONE) {
        break;
      }
      if (res == ClientSession::ReadResult::MALFORMED) {
        close_it = true;  // protocol violation: drop the client
        break;
      }
      m_staging.at(m_staged++) = cmd;
      if (m_staged == k_stage_batch) {
        SubmitStaged();
      }
    }

    if (close_it || fill == ClientSession::FillResult::CLOSED) {
      close_it = true;
      break;
    }
    if (fill == ClientSession::FillResult::WOULD_BLOCK) {
      break;  // socket drained (or buffer momentarily full); done for now
    }
    // GOT_DATA: there may be more on the socket, keep going
  }

  // Flush whatever valid orders are staged before we act on a close, so a
  // client's last good orders are never lost to a trailing bad byte or FIN
  SubmitStaged();

  if (close_it) {
    ::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, session.Fd(), nullptr);
    session.Close();
  }
}

void ClientIo::Run(const std::stop_token& stop) {
  std::array<epoll_event, k_epoll_max_events> events{};

  while (!stop.stop_requested()) {
    // Finite timeout: request_stop() does not wake epoll_wait, so the
    // loop must surface every k_io_poll_timeout_ms to check the token,
    // which bounds how long Stop()'s join can take
    const int n{::epoll_wait(m_epoll_fd, events.data(),
                             static_cast<int>(events.size()),
                             k_io_poll_timeout_ms)};
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }       // interrupted by a signal
      break;  // unexpected epoll failure
    }

    for (auto i{0}; i < n; ++i) {
      const epoll_event& ev{events.at(static_cast<std::size_t>(i))};

      // nullptr tag == the listener; any other tag is that session
      if (ev.data.ptr == nullptr) {
        OnAcceptable();
        continue;
      }

      auto& session{*static_cast<ClientSession*>(ev.data.ptr)};

      // A hangup/error means the peer is gone. Stop polling this fd
      // (else EPOLLHUP re-fires every wait and spins the loop) and
      // mark the session closed; the fd is released by ~ClientSession
      if ((ev.events & (EPOLLHUP | EPOLLERR)) != 0) {
        ::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, session.Fd(), nullptr);
        session.Close();
        continue;
      }

      OnReadable(session);
    }
  }
}

}  // namespace lfob
