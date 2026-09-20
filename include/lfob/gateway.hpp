#ifndef GATEWAY_HPP_
#define GATEWAY_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "client_io.hpp"
#include "feed_handler.hpp"
#include "gateway_config.hpp"
#include "matching_engine.hpp"
#include "output_worker.hpp"

namespace lfob {

// The gateway is everything between the engine and the network
//
//        ITCH multicast ──► FeedHandler ─┐
//                                        ├─► MpscRing ─► matching thread
//        client TCP ──────► ClientIo ────┘                     │
//                                                              ▼
//        client TCP ◄────── OutputWorker xN ◄────────────── SpmcRing
//
//   ingress   lossy. MpscRing::TryPush fails under overload and the client
//             gets an INGRESS_FULL reject. Refusing an order is a normal,
//             reportable outcome; queueing it unboundedly is not
//
//   egress    lossless inside the process, SpmcRing applies back-pressure
//             so no output worker misses a report -- and lossy at the socket.
//             A client that cannot keep up gets its public market data
//             dropped and a gap notice, because stalling a socket write
//             would push back through the worker, into the ring, and onto the
//             matching thread. One slow TCP receiver must never be able to
//             reach the book
//
// The pieces live in their own headers -- client_session, output_worker,
// client_io, feed_handler -- with the shared tuning knobs in gateway_config.
// This header ties them together behind the Gateway facade.

// <---- Facade ---->

// Owns the whole front end and its startup order, which matters: workers
// register their egress cursors before the engine publishes anything, because
// SpmcRing::Register is single-threaded setup and a consumer that joins late
// would silently miss everything published before it
class Gateway {
 public:
  struct Config {
    std::uint16_t listen_port;
    int io_cpu_core;
    std::size_t worker_count;
    std::array<int, k_max_output_workers> worker_cpu_cores;
    bool enable_feed;
    FeedHandler::Config feed;
  };

  Gateway(MatchingEngine& engine, const Config& config);
  ~Gateway();

  Gateway(const Gateway&) = delete;
  Gateway(Gateway&&) = delete;
  Gateway& operator=(const Gateway&) = delete;
  Gateway& operator=(Gateway&&) = delete;

  // Registers workers, binds the listener, joins the feed, then starts every
  // thread. Returns false without starting anything if any step fails
  bool Start();

  // Reverse order: stop intake first so the engine drains, then retire the
  // workers so they leave the broadcast cleanly rather than being evicted
  void Stop() noexcept;

  // Aggregated monitoring
  [[nodiscard]] std::uint64_t ClientDrops() const noexcept;
  [[nodiscard]] std::uint64_t WorkerEvictions() const noexcept;

 private:
  MatchingEngine& m_engine;
  Config m_config;
  ClientIo m_io;

  // Workers are neither copyable nor movable -- they own a thread and an
  // egress cursor -- so they are constructed in place and never relocated.
  // A vector would need them move-insertable just to grow
  std::array<std::optional<OutputWorker>, k_max_output_workers> m_workers{};

  FeedHandler m_feed;
  bool m_started{false};
};

}  // namespace lfob

#endif  // GATEWAY_HPP_
