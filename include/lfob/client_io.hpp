#ifndef CLIENT_IO_HPP_
#define CLIENT_IO_HPP_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

#include "client_session.hpp"
#include "matching_engine.hpp"
#include "output_worker.hpp"
#include "types.hpp"

namespace lfob {

// <---- Ingress: client order intake ---->

// Accepts connections, parses inbound orders and submits them. One thread
// owns the listening socket and every session's read side; the write side
// belongs to whichever OutputWorker adopted the session
class ClientIo {
 public:
  ClientIo(MatchingEngine& engine, int cpu_core) noexcept;
  ~ClientIo();

  ClientIo(const ClientIo&) = delete;
  ClientIo(ClientIo&&) = delete;
  ClientIo& operator=(const ClientIo&) = delete;
  ClientIo& operator=(ClientIo&&) = delete;

  bool Listen(std::uint16_t port) noexcept;
  void Start();
  void Stop() noexcept;

  // Where accepted sessions go. Round-robins across the registered workers
  void AddWorker(OutputWorker& worker) noexcept;

  [[nodiscard]] std::uint64_t Accepted() const noexcept;
  [[nodiscard]] std::uint64_t Rejected() const noexcept;

 private:
  void ConfigureThread() const noexcept;
  void Run(const std::stop_token& stop);

  void OnAcceptable() noexcept;
  void OnReadable(ClientSession& session) noexcept;

  // Parse whatever whole messages are buffered into staging, then push the
  // batch. SubmitBulk reports a shortfall when ingress is full; every command
  // past that point owes its client an INGRESS_FULL reject, which is emitted
  // here rather than by the engine -- the engine never saw them
  std::size_t SubmitStaged() noexcept;
  void RejectForBackpressure(const OrderCommand& cmd) noexcept;

  MatchingEngine& m_engine;
  int m_cpu_core;
  int m_listen_fd{-1};
  int m_epoll_fd{-1};

  // Sole owner of every session. Handed to a worker as a raw pointer, which
  // is safe only because this vector outlives every worker thread: entries
  // are never erased while the gateway is running, and a disconnected client
  // leaves a CLOSED shell behind until Stop()
  std::vector<std::unique_ptr<ClientSession>> m_sessions;

  std::vector<OutputWorker*> m_workers;
  std::size_t m_next_worker{0};
  ClientId m_next_client{1};

  std::array<OrderCommand, k_stage_batch> m_staging{};
  std::size_t m_staged{0};
  std::jthread m_thread;

  std::atomic<std::uint64_t> m_accepted{0};
  std::atomic<std::uint64_t> m_rejected{0};
};

}  // namespace lfob

#endif  // CLIENT_IO_HPP_
