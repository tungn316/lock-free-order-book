#include <lfob/cpu_relax.h>
#include <lfob/affinity.hpp>
#include <lfob/clock.hpp>
#include <lfob/matching_engine.hpp>

#include <algorithm>
#include <array>

namespace lfob {

MatchingEngine::MatchingEngine(Price min_price, Price max_price, int cpu_core)
    : m_book(min_price, max_price, k_max_orders, *this),
      m_cpu_core(cpu_core),
      m_ingress_rejects(0),
      m_spin_iterations(0) {}

MatchingEngine::~MatchingEngine() {
  Stop();
}

// <---- Ingress: any I/O thread ---->

bool MatchingEngine::Submit(const OrderCommand& cmd) noexcept {
  OrderCommand stamped{cmd};
  stamped.ingress_ts = NowNanos();
  if (m_ingress.TryPush(stamped)) {
    return true;
  }
  m_ingress_rejects.fetch_add(1, std::memory_order::relaxed);
  return false;
}

std::size_t MatchingEngine::SubmitBulk(const OrderCommand* cmds,
                                       std::size_t count) noexcept {
  // Each command needs its ingress_ts stamped. Since cmds is
  // const and TryPushBulk needs a contiguous, writable buffer,
  // copy through a bounded on-stack staging buffer in chunks:
  // - doesn't mutate the caller's array
  // - doesn't allocate
  std::array<OrderCommand, k_stage_batch> staged{};

  std::size_t pushed{0};
  while (pushed < count) {
    const std::size_t chunk{std::min(k_stage_batch, count - pushed)};
    for (auto i{0UZ}; i < chunk; ++i) {
      staged.at(i) = cmds[pushed + i];
      staged.at(i).ingress_ts = NowNanos();
    }
    // Short count means the ring filled up; stop and report the shortfall.
    const std::size_t n{m_ingress.TryPushBulk(staged.data(), chunk)};
    pushed += n;
    // Pushed less than the expected chunk amount and exit early
    if (n < chunk) {
      break;
    }
  }

  if (pushed < count) {
    m_ingress_rejects.fetch_add(count - pushed, std::memory_order::relaxed);
  }
  return pushed;
}

// <---- Egress: output worker threads ---->

MatchingEngine::ConsumerId MatchingEngine::RegisterOutputWorker() {
  return m_egress.Register();
}

std::size_t MatchingEngine::ReadReports(ConsumerId id,
                                        ExecutionReport* out,
                                        std::size_t max) noexcept {
  return m_egress.TryReadBulk(id, out, max);
}

bool MatchingEngine::IsOutputWorkerLive(ConsumerId id) const noexcept {
  return m_egress.IsAttached(id);
}

bool MatchingEngine::RejoinOutputWorker(ConsumerId id) noexcept {
  return m_egress.Rejoin(id);
}

void MatchingEngine::RetireOutputWorker(ConsumerId id) noexcept {
  m_egress.Leave(id);
}

// <---- Any reader thread ---->

Bbo MatchingEngine::GetBbo() const noexcept {
  return m_bbo.Load();
}

std::uint64_t MatchingEngine::IngressRejects() const noexcept {
  return m_ingress_rejects.load(std::memory_order::relaxed);
}

std::uint64_t MatchingEngine::EgressEvictions() const noexcept {
  return m_egress.Evictions();
}

// <---- Lifecycle ---->

// Runs once on startup. Moves every page fault and swap risk off the hot path:
// - Pin all current and future pages for this process in RAM
// - Explicitly touch this object's inline ring buffers
//   (default-initialized and so not yet backed by physical frames)
// - Node arena's vectors are already first-touched by resize
void MatchingEngine::PrepareMemory() noexcept {
  LockMemory();
  Prefault(this, sizeof(*this));
}

// Runs on matching thread:
// - Scheduling class and CPU affinity are per-thread, so they cannot be set
// from Start()
// - Pin first so the thread stops migrating
// - Lift into SCHED_FIFO above the normal-class threads
void MatchingEngine::ConfigureThread() const noexcept {
  PinCurrentThread(m_cpu_core);
  SetRealtimePriority(k_matching_rt_priority);
}

void MatchingEngine::Start() {
  PrepareMemory();
  m_thread = std::jthread([this](const std::stop_token& stop) { Run(stop); });
}

void MatchingEngine::Stop() noexcept {
  m_thread.request_stop();
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

// <---- Matching thread ---->

void MatchingEngine::Run(const std::stop_token& stop) {
  ConfigureThread();

  while (!stop.stop_requested()) {
    const std::size_t n{Poll()};
    m_spin_iterations.fetch_add(1, std::memory_order::relaxed);
    if (n == 0) {
      CpuRelax();  // hint to processor that we are in spin-lock
    }
  }

  // Drain whatever is still queued so no accepted command is lost on shutdown
  while (Poll() > 0) {
  }
}

std::size_t MatchingEngine::Poll() noexcept {
  std::array<OrderCommand, k_drain_batch> batch{};
  const std::size_t n{m_ingress.TryPopBulk(batch.data(), batch.size())};
  for (std::size_t i{0}; i < n; ++i) {
    m_book.Apply(batch.at(i));
  }

  // Drain synthetic reports (e.g. backpressure rejects the I/O threads could
  // not deliver themselves). Publishing here is what keeps the egress ring
  // single-producer: only this thread ever emits. The book stamps each with the
  // next egress sequence, so the injected reports slot cleanly into the stream
  std::size_t injected{0};
  for (std::optional<ExecutionReport> r{m_inject.TryPop()}; r.has_value();
       r = m_inject.TryPop()) {
    m_book.EmitInjected(*r);
    ++injected;
  }

  return n + injected;
}

bool MatchingEngine::InjectReport(const ExecutionReport& report) noexcept {
  return m_inject.TryPush(report);
}

// Called only from the matching thread, inside OrderBook::Apply
void MatchingEngine::Emit(const ExecutionReport& report_in) {
  // Single stamp point for every report type: this funnel runs on the matching
  // thread, so match_ts marks when the engine finished producing this event.
  // A multi-fill sweep therefore gets a slightly increasing match_ts per fill,
  // which is the true per-event completion time
  ExecutionReport report{report_in};
  report.match_ts = NowNanos();

  // Publish fresh top-of-book to the seqlock before broadcasting so BBO
  // readers see the new quote without waiting on a possibly-lagging consumer
  if (report.type == ExecutionReport::Type::TOP_OF_BOOK) {
    m_bbo.Store(Bbo{
        .event_seq = report.seq,
        .bid_price = report.bid_price,
        .bid_quantity = report.bid_quantity,
        .ask_price = report.ask_price,
        .ask_quantity = report.ask_quantity,
    });
  }

  // Egress is lossless with back-pressure, so a full ring means waiting on the
  // slowest worker. Common case: it has room, one store, done
  if (!m_egress.TryPush(report)) {
    PublishOrEvict(report);
  }
}

// Back-pressure from a live worker is a feature (it's what makes egress
// lossless) back-pressure from a dead one is an outage. This thread is the
// only thing draining ingress, so a worker that never advances its cursor
// would stop the book, fill the ingress ring and turn every client order into
// an INGRESS_FULL reject -- one dead consumer taking down the exchange.
//
// So the wait is bounded. Once the grace period expires, whoever is holding
// the gate is evicted and the engine moves on. The victim keeps its
// registration and finds out on its next read that it has been detached
void MatchingEngine::PublishOrEvict(const ExecutionReport& report) noexcept {
  Timestamp deadline{NowNanos() + k_egress_stall_budget_ns};
  std::uint32_t spins{0};

  while (!m_egress.TryPush(report)) {
    CpuRelax();  // hint to processor that we are in spin-lock
    if (++spins < k_egress_stall_poll_spins) {
      continue;
    }
    spins = 0;
    if (NowNanos() < deadline) {
      continue;
    }

    // Evicting frees the gate, but another worker may be stalled right behind
    // it. Restart the budget so the next one is judged on its own merits
    // rather than inheriting a deadline that has already expired
    const ConsumerId victim{m_egress.BlockingConsumer()};
    if (victim.idx != k_invalid_consumer) {
      m_egress.Evict(victim);
    }
    deadline = NowNanos() + k_egress_stall_budget_ns;
  }
}

}  // namespace lfob
