#ifndef MATCHING_ENGINE_HPP_
#define MATCHING_ENGINE_HPP_

#include <atomic>
#include <cstddef>
#include <stop_token>
#include <thread>
#include "execution_report.hpp"
#include "mpsc_ring.hpp"
#include "order_book.hpp"
#include "seqlock.hpp"
#include "spmc_ring.hpp"

namespace lfob {

inline constexpr std::size_t k_ingress_capacity{1U << 16U};
inline constexpr std::size_t k_egress_capacity{1U << 16U};

// Synthetic reports injected by I/O threads (e.g. INGRESS_FULL rejects for
// commands the engine never saw). Drained and published by the matching thread,
// so the egress ring keeps its single producer
inline constexpr std::size_t k_inject_capacity{1U << 10U};
inline constexpr std::size_t k_max_orders{1U << 20U};  // ~1M nodes
inline constexpr std::size_t k_drain_batch{64};

// SCHED_FIFO priority for the matching thread. High enough to outrank the
// normal-class crowd outright, but below 99 to leave headroom for kernel
// IRQ/watchdog threads that must still be able to preempt us.
inline constexpr int k_matching_rt_priority{80};

// How long the matching thread will wait on an output worker that has stopped
// draining before evicting it from the egress broadcast. Sized between two
// hard bounds:
//
//   floor   longer than the worst hiccup of a *healthy* worker, or normal
//           jitter starts evicting good workers. A pinned SCHED_FIFO worker
//           stalls for tens of microseconds; one left in the normal class can
//           lose a full CFS slice (~1-4 ms), so raise this if the output
//           workers are not real-time.
//   ceiling short enough that the stall cannot overflow ingress:
//           k_ingress_capacity / peak order rate. At 65536 slots and 1M
//           orders/s that is ~65 ms, so stay an order of magnitude under it.
//
// The budget is per victim, not per Emit: after an eviction the clock restarts
// so each worker gets its own grace period. Worst case with every worker
// wedged at once is therefore MAX_CONSUMERS * this.
inline constexpr Timestamp k_egress_stall_budget_ns{2'000'000};  // 2 ms

// A clock read is ~20ns through the vDSO, which is not free inside a spin
// loop. Sample the deadline once every N pause instructions instead
inline constexpr std::uint32_t k_egress_stall_poll_spins{256};

// Owns book, ingress MPSC ring, egress SPMC broadcast ring,
// BBO seqlock and pinned matching thread
class MatchingEngine final : public ReportSink {
 public:
  MatchingEngine(Price min_price, Price max_price, int cpu_core);
  ~MatchingEngine() override;

  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine(MatchingEngine&&) = delete;

  MatchingEngine& operator=(const MatchingEngine&) = delete;
  MatchingEngine& operator=(MatchingEngine&&) = delete;

  // <---- Ingress: any I/O thread ---->
  bool Submit(const OrderCommand& cmd) noexcept;
  std::size_t SubmitBulk(const OrderCommand* cmds, std::size_t count) noexcept;

  // <---- Egress: output worker threads ---->
  using Egress = SpmcRing<ExecutionReport, k_egress_capacity>;
  using ConsumerId = Egress::ConsumerId;

  ConsumerId RegisterOutputWorker();
  std::size_t ReadReports(ConsumerId id,
                          ExecutionReport* out,
                          std::size_t max) noexcept;

  // Thread-safe from any I/O thread. Queues a synthetic report; the matching
  // thread stamps its seq and publishes it onto the egress ring, so the ring
  // stays single-producer. Returns false if the inject queue is full. The
  // caller leaves seq/match_ts unset -- the matching thread assigns them
  bool InjectReport(const ExecutionReport& report) noexcept;

  // A worker that stops draining is evicted so it cannot hold the matching
  // thread hostage (see k_egress_stall_budget_ns). ReadReports then returns 0
  // forever, which is indistinguishable from an idle feed, so a worker must
  // check this whenever it comes up empty.
  //
  // The recovery is deliberately not a replay: Rejoin puts the worker back on
  // the live edge and the reports it missed are gone. It owes its clients a
  // gap notice plus a fresh snapshot -- GetBbo() is exactly that, and it is
  // readable without touching the ring
  [[nodiscard]] bool IsOutputWorkerLive(ConsumerId id) const noexcept;
  bool RejoinOutputWorker(ConsumerId id) noexcept;
  void RetireOutputWorker(ConsumerId id) noexcept;  // graceful, on shutdown

  // <---- Any reader thread ---->
  [[nodiscard]] Bbo GetBbo() const noexcept;

  // Monitoring. Never read on the hot path
  [[nodiscard]] std::uint64_t IngressRejects() const noexcept;
  [[nodiscard]] std::uint64_t EgressEvictions() const noexcept;

  void Start();
  void Stop() noexcept;

  void Emit(const ExecutionReport& report) override;  // matching thread only

 private:
  void PrepareMemory() noexcept;   // lock + prefault, before the thread starts
  void ConfigureThread() const noexcept;  // pin + priority, on the thread
  void Run(const std::stop_token& stop);  // pin, then spin forever
  std::size_t Poll() noexcept;     // drain <= kDrainBatch, apply each

  // Emit's slow path: block on a full egress ring, but only for a bounded
  // grace period per lagging worker
  void PublishOrEvict(const ExecutionReport& report) noexcept;

  MpscRing<OrderCommand, k_ingress_capacity> m_ingress;
  MpscRing<ExecutionReport, k_inject_capacity> m_inject;
  SpmcRing<ExecutionReport, k_egress_capacity> m_egress;
  Seqlock<Bbo> m_bbo;

  OrderBook m_book;
  int m_cpu_core;
  std::jthread m_thread;

  // Monitoring only - never read on the hot path
  std::atomic<std::uint64_t> m_ingress_rejects;
  std::atomic<std::uint64_t> m_spin_iterations;
};

}  // namespace lfob

#endif  // MATCHING_ENGINE_HPP_
