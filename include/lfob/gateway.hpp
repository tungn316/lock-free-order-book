#ifndef GATEWAY_HPP_
#define GATEWAY_HPP_

#include <cstddef>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>
#include "matching_engine.hpp"

namespace lfob {

// <---- Ingress ---->

class Session {
 public:
  Session(ClientId id, int fd);

  std::size_t Parse(std::span<const std::byte> bytes,
                    std::span<OrderCommand> out);
  [[nodiscard]] bool Validate(const OrderCommand& cmd) const;

  [[nodiscard]] ClientId Id() const noexcept;
  [[nodiscard]] int Fd() const noexcept;

 private:
  ClientId m_id;
  int m_fd;
  std::vector<std::byte> m_inbuf;
};

class IoThread {
 public:
  IoThread(MatchingEngine& engine, int cpu_core);

  void AddSession(Session session);
  void Start();
  void Stop() noexcept;

 private:
  void Run(std::stop_token stop);
  void RejectForBackpressure(Session& session, const OrderCommand& cmd);

  MatchingEngine* m_engine;
  int m_cpu_core;
  int m_epoll_fd;
  std::vector<Session> m_sessions;
  std::array<OrderCommand, k_drain_batch> m_staging;
  std::jthread m_thread;
};

// <---- Egress ---->

class OutputWorker {
 public:
  enum class Transport : std::uint8_t { TCP_DROP, UDP_MULTICAST };

  OutputWorker(MatchingEngine& engine, Transport transport, int cpu_core);

  void Start();
  void Stop() noexcept;

  // Reports lost because this worker fell more than kEgressCapacity
  // behind. Surfacing this is mandatory: a silent gap in a market data
  // feed is worse than a visible one.
  [[nodiscard]] std::uint64_t Missed() const noexcept;

 private:
  void Run(std::stop_token stop);
  void Serialise(const ExecutionReport& report);
  void Flush();

  MatchingEngine* m_engine;
  MatchingEngine::ConsumerId m_consumer;
  Transport m_transport;
  int m_cpu_core;
  std::array<ExecutionReport, k_drain_batch> m_staging;
  std::vector<std::byte> m_outbuf;
  std::jthread m_thread;
};

class Gateway {
 public:
  Gateway(MatchingEngine& engine,
          std::span<const int> io_cores,
          std::span<const int> output_cores);

  void Start();
  void Stop() noexcept;

 private:
  MatchingEngine* m_engine;
  std::vector<IoThread> m_io;
  std::vector<OutputWorker> m_output;
};

}  // namespace lfob

#endif  // GATEWAY_HPP_
