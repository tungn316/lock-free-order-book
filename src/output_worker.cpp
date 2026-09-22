#include <lfob/output_worker.hpp>

#include <lfob/cpu_relax.h>
#include <lfob/affinity.hpp>
#include <lfob/report_wire.hpp>

#include <cassert>
#include <optional>

namespace lfob {

OutputWorker::OutputWorker(MatchingEngine& engine, int cpu_core) noexcept
    : m_engine(engine), m_cpu_core(cpu_core) {
  m_sessions.reserve(k_max_client_sessions);
}

OutputWorker::~OutputWorker() {
  Stop();
}

bool OutputWorker::Register() noexcept {
  m_id = m_engine.RegisterOutputWorker();
  return (m_id.idx != k_invalid_consumer);
}

bool OutputWorker::Adopt(ClientSession* session) noexcept {
  // Thread-safe handoff. ClientIo (the single producer) enqueues; the
  // worker drains its inbox at the top of every Poll and moves sessions
  // into m_sessions -- so m_sessions is only ever mutated by the worker
  // thread and the fan-out stays lock-free. Works before OR after Start:
  // a pre-start Adopt is simply picked up by the first Poll. Returns false
  // on a full inbox so ClientIo rejects the client rather than dropping it
  return session != nullptr && m_inbox.TryPush(session);
}

void OutputWorker::DrainInbox() noexcept {
  // Single-consumer side: only this worker thread touches m_sessions
  for (std::optional<ClientSession*> s{m_inbox.TryPop()}; s;
       s = m_inbox.TryPop()) {
    m_sessions.push_back(*s);
  }
}

void OutputWorker::Start() {
  assert(m_id.idx != k_invalid_consumer && "Register() before Start()");
  m_thread = std::jthread([this](const std::stop_token& stop) {
    ConfigureThread();
    Run(stop);
  });
}

void OutputWorker::Stop() noexcept {
  if (m_thread.joinable()) {
    m_thread.request_stop();
  }

  if (m_id.idx != k_invalid_consumer) {
    m_engine.RetireOutputWorker(m_id);
    m_id.idx = k_invalid_consumer;
  }

  if (m_thread.joinable()) {
    m_thread.join();
  }
}

[[nodiscard]] bool OutputWorker::IsLive() const noexcept {
  // The engine is the authority: it can detach a worker for stalling past
  // the 2 ms budget, and from inside Run that is invisible. "Live" means
  // "still attached to the broadcast," which only the ring knows. Guard
  // the invalid id so a never-registered / retired worker reads not-live
  return m_id.idx != k_invalid_consumer && m_engine.IsOutputWorkerLive(m_id);
}

[[nodiscard]] std::uint64_t OutputWorker::Evictions() const noexcept {
  return m_evictions.load(std::memory_order::relaxed);
}

[[nodiscard]] std::uint64_t OutputWorker::Dropped() const noexcept {
  return m_dropped.load(std::memory_order::relaxed);
}

[[nodiscard]] std::uint64_t OutputWorker::ReportsSent() const noexcept {
  return m_reports_sent.load(std::memory_order::relaxed);
}

void OutputWorker::ConfigureThread() const noexcept {
  PinCurrentThread(m_cpu_core);
  SetRealtimePriority(k_output_rt_priority);
}

void OutputWorker::Run(const std::stop_token& stop) {
  while (!stop.stop_requested()) {
    const std::size_t handled{Poll()};

    if (handled > 0) {
      m_idle_polls = 0;
      continue;
    }

    if (++m_idle_polls >= k_idle_polls_before_liveness_check) {
      m_idle_polls = 0;
      CheckLiveness();
    }
    CpuRelax();
  }
}

std::size_t OutputWorker::Poll() noexcept {
  // 0. Pick up any sessions ClientIo handed off since the last pass
  DrainInbox();

  // 1. Drain a batch off the broadcast ring through OUR cursor (m_id)
  const std::size_t n{
      m_engine.ReadReports(m_id, m_batch.data(), m_batch.size())};

  // 2. Fan each report out to whichever sessions should see it
  for (std::size_t i{0}; i < n; ++i) {
    Dispatch(m_batch.at(i));
  }

  // 3. Push staged bytes to every socket, then drop dead sessions
  FlushAll();
  Reap();

  return n;
}

void OutputWorker::Dispatch(const ExecutionReport& report) noexcept {
  const Route route{RouteOf(report)};

  if (route == Route::PRIVATE || route == Route::BOTH) {
    SendToOwner(report);
  }

  if (route == Route::PUBLIC || route == Route::BOTH) {
    const std::size_t n{EncodeReport(report, m_frames.data(), m_frames.size())};
    if (n > 0) {
      Broadcast(m_frames.data(), n);
    }
  }

  m_reports_sent.fetch_add(1, std::memory_order::relaxed);
}

void OutputWorker::SendToOwner(const ExecutionReport& report) noexcept {
  for (auto* s : m_sessions) {
    if (s->Id() == report.client) {
      const std::size_t n{
          EncodeReport(report, m_frames.data(), m_frames.size())};
      if (n > 0) {
        s->PublishPrivate(m_frames.data(), n);
      }
      return;
    }
  }
}

void OutputWorker::Broadcast(const std::byte* frame,
                             std::size_t bytes) noexcept {
  for (ClientSession* s : m_sessions) {
    s->PublishPublic(frame, bytes);
  }
}

void OutputWorker::FlushAll() noexcept {
  const Bbo bbo{m_engine.GetBbo()};  // read once via seqlock, reuse
  for (ClientSession* s : m_sessions) {
    s->Flush();  // <-- YOUR writev drain

    // Flushing may have freed room. A GAPPED session owes a repair, so try
    // to stage it now: gap notice, then snapshot, then it flips ACTIVE.
    if (s->NeedsResync()) {             // <-- YOUR predicate
      s->SendGapNotice(bbo.event_seq);  // <-- YOUR methods
      s->SendSnapshot(bbo);
    }
  }
}

void OutputWorker::Reap() noexcept {
  std::erase_if(m_sessions, [](const ClientSession* s) {
    return s->GetState() == ClientSession::State::CLOSED;
  });
}

bool OutputWorker::CheckLiveness() noexcept {
  if (m_engine.IsOutputWorkerLive(m_id)) {
    return true;
  }
  // We were detached for stalling past the 2 ms budget. Everything published
  // while we were gone is lost.
  m_evictions.fetch_add(1, std::memory_order::relaxed);  // your counter
  m_engine.RejoinOutputWorker(m_id);  // back onto the LIVE edge (no replay)
  Resync();
  return false;
}

void OutputWorker::Resync() noexcept {
  const Bbo bbo{m_engine.GetBbo()};
  for (ClientSession* s : m_sessions) {
    s->MarkGapped();
    s->SendGapNotice(bbo.event_seq);
    s->SendSnapshot(bbo);
  }
}

}  // namespace lfob
