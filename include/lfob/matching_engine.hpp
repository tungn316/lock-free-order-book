#ifndef MATCHING_ENGINE_HPP_
#define MATCHING_ENGINE_HPP_

#include <atomic>
#include <cstddef>
#include <stop_token>
#include <thread>
#include "execution_report.hpp"
#include "mpsc_queue.hpp"
#include "order_book.hpp"
#include "seqlock.hpp"
#include "spmc_ring.hpp"

namespace lfob {

inline constexpr std::size_t k_ingress_capacity = 1U << 16U;
inline constexpr std::size_t k_egress_capacity = 1U << 16U;
inline constexpr std::size_t k_max_orders = 1U << 20U;  // ~1M nodes
inline constexpr std::size_t k_drain_batch = 64;

// Owns the book, the ingress MPSC ring, the egress SPMC broadcast ring,
// the BBO seqlock, and the pinned matching thread.
class MatchingEngine final : public ReportSink {
 public:
  MatchingEngine(Price min_price, Price max_price, int cpu_core);
  ~MatchingEngine() override;

  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine(MatchingEngine&&) = delete;

  MatchingEngine& operator=(const MatchingEngine&) = delete;
  MatchingEngine& operator=(MatchingEngine&&) = delete;

  // --- Ingress: any I/O thread ---
  bool Submit(const OrderCommand& cmd) noexcept;
  std::size_t SubmitBulk(const OrderCommand* cmds, std::size_t count) noexcept;

  // --- Egress: output worker threads, registered before start() ---
  using ConsumerId = SpmcRing<ExecutionReport, k_egress_capacity>::ConsumerId;
  ConsumerId RegisterOutputWorker();
  std::size_t ReadReports(ConsumerId id,
                          ExecutionReport* out,
                          std::size_t max) noexcept;

  // --- Any reader thread ---
  Bbo GetBbo() const noexcept;

  void Start();
  void Stop() noexcept;

 private:
  void Run(std::stop_token stop);  // pin, then spin forever
  std::size_t Poll() noexcept;     // drain <= kDrainBatch, apply each

  void Emit(const ExecutionReport& report) override;  // matching thread only

  MpscQueue<OrderCommand, k_ingress_capacity> m_ingress;
  SpmcRing<ExecutionReport, k_egress_capacity> m_egress;
  Seqlock<Bbo> m_bbo;

  OrderBook m_book;
  int m_cpu_core;
  std::jthread m_thread;

  // Monitoring only; never read on the hot path.
  std::atomic<std::uint64_t> m_ingress_rejects;
  std::atomic<std::uint64_t> m_spin_iterations;
};

}  // namespace lfob

#endif  // MATCHING_ENGINE_HPP_
