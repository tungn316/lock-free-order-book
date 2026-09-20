#ifndef OUTPUT_WORKER_HPP_
#define OUTPUT_WORKER_HPP_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <thread>
#include <vector>

#include "client_session.hpp"
#include "execution_report.hpp"
#include "gateway_config.hpp"
#include "matching_engine.hpp"
#include "mpsc_ring.hpp"
#include "types.hpp"

namespace lfob {

// <---- Egress: output workers ---->

// Drains the engine's broadcast ring and fans it out to its own sessions.
// Sessions are partitioned across workers, never shared, so nothing here is
// synchronized: one thread, its own consumer cursor, its own set of sockets.
//
// <---- Surviving eviction ---->
//
// The engine will detach this worker if it stops draining for longer than
// k_egress_stall_budget_ns (see matching_engine.hpp). From inside the worker
// that is invisible -- ReadReports simply returns 0, exactly like an idle
// market -- so the loop has to ask. After k_idle_polls_before_liveness_check
// empty reads it calls IsOutputWorkerLive, and if it has been detached:
//
//   1. every session it owns is marked GAPPED. Reports were published while
//      we were not reading them and they are gone; pretending otherwise
//      would leave clients with a book that silently diverges
//   2. RejoinOutputWorker puts the cursor back on the live edge. Not a
//      replay: whatever was missed stays missed, which is what makes the
//      recovery bounded
//   3. each session gets a gap notice and then a fresh snapshot built from
//      MatchingEngine::GetBbo(). The seqlock exists precisely so this path
//      can read top-of-book without touching the ring it was just thrown off
//
// Steps 2 and 3 are ordered that way on purpose. Rejoining first means the
// snapshot is taken *after* the cursor is live, so any report that changes
// the book between snapshot and resume is one this worker will still receive.
// The reverse order leaves a hole the client can never detect
class OutputWorker {
 public:
  OutputWorker(MatchingEngine& engine, int cpu_core) noexcept;
  ~OutputWorker();

  OutputWorker(const OutputWorker&) = delete;
  OutputWorker(OutputWorker&&) = delete;
  OutputWorker& operator=(const OutputWorker&) = delete;
  OutputWorker& operator=(OutputWorker&&) = delete;

  // Before Start(): claim an egress cursor and take ownership of sessions
  bool Register() noexcept;
  bool Adopt(ClientSession* session) noexcept;

  void Start();
  void Stop() noexcept;  // graceful: RetireOutputWorker, then join

  [[nodiscard]] bool IsLive() const noexcept;
  [[nodiscard]] std::uint64_t Evictions() const noexcept;
  [[nodiscard]] std::uint64_t Dropped() const noexcept;
  [[nodiscard]] std::uint64_t ReportsSent() const noexcept;

 private:
  void ConfigureThread() const noexcept;
  void Run(const std::stop_token& stop);

  // One pass: drain the ring, fan out, flush every socket. Returns reports
  // handled so the loop knows whether it was idle
  std::size_t Poll() noexcept;

  void Dispatch(const ExecutionReport& report) noexcept;
  void SendToOwner(const ExecutionReport& report) noexcept;
  void Broadcast(const std::byte* frame, std::size_t bytes) noexcept;
  void FlushAll() noexcept;

  // Drop CLOSED sessions from the fan-out. Does not destroy them: ClientIo
  // owns them and reclaims them once no worker can reach them
  void Reap() noexcept;

  // Move any sessions ClientIo handed off into m_sessions. Called at the top of
  // Poll so the fan-out set is only ever mutated by this worker thread
  void DrainInbox() noexcept;

  // Called when Poll() came up empty often enough to be suspicious. Returns
  // true if the worker is still attached; performs the full resync if not
  bool CheckLiveness() noexcept;
  void Resync() noexcept;

  MatchingEngine& m_engine;
  MatchingEngine::ConsumerId m_id{k_invalid_consumer};
  int m_cpu_core;

  std::vector<ClientSession*> m_sessions;  // mutated only by the worker thread
  std::array<ExecutionReport, k_report_drain_batch> m_batch{};
  std::array<std::byte, k_worker_frame_bytes> m_frames{};

  // Single-producer (ClientIo) / single-consumer (this worker) handoff of
  // newly-accepted sessions. Drained by DrainInbox() inside Poll
  MpscRing<ClientSession*, k_worker_handoff_slots> m_inbox;

  std::uint32_t m_idle_polls{0};
  std::jthread m_thread;

  // Monitoring, read from outside the worker thread
  std::atomic<std::uint64_t> m_evictions{0};
  std::atomic<std::uint64_t> m_dropped{0};
  std::atomic<std::uint64_t> m_reports_sent{0};
};

}  // namespace lfob

#endif  // OUTPUT_WORKER_HPP_
