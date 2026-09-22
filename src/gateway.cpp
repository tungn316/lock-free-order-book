#include <lfob/gateway.hpp>

namespace lfob {

// <---- Gateway (facade) ---->

Gateway::Gateway(MatchingEngine& engine, const Config& config)
    : m_engine(engine),
      m_config(config),
      m_io(engine, config.io_cpu_core),
      m_feed(engine, config.feed) {
  // Workers own a thread and an egress cursor, so they cannot be copied or
  // moved into the array -- construct exactly worker_count of them in
  // place. Clamp to the array bound so a misconfigured count can't run off
  // the end
  const std::size_t count{config.worker_count < k_max_output_workers
                              ? config.worker_count
                              : k_max_output_workers};
  for (std::size_t i{0}; i < count; ++i) {
    m_workers.at(i).emplace(engine, config.worker_cpu_cores.at(i));
  }
}

Gateway::~Gateway() {
  // Stop() is idempotent and performs the ordered teardown (intake before
  // workers) while every member is still alive. Once it returns, all
  // threads are joined, so the member destructors that follow are quiet
  Stop();
}

bool Gateway::Start() {
  if (m_started) {
    return true;
  }

  // --- Phase 1: fallible setup, before any thread runs ---
  // A failure here returns false with nothing started, so there is nothing
  // to unwind. Workers claim their egress cursors FIRST: SpmcRing::Register
  // is single-threaded setup, and a consumer that joins after the engine
  // has begun publishing would silently miss every report emitted before it
  for (auto& slot : m_workers) {
    if (slot.has_value() && !slot->Register()) {
      return false;
    }
  }
  if (!m_io.Listen(m_config.listen_port)) {
    return false;
  }
  if (m_config.enable_feed && !m_feed.Join()) {
    return false;
  }

  // --- Phase 2: wire the fan-out ---
  // Point ClientIo at every worker so accepted sessions round-robin across
  // them
  for (auto& slot : m_workers) {
    if (slot.has_value()) {
      m_io.AddWorker(*slot);
    }
  }

  // --- Phase 3: launch threads ---
  // Workers first, so they are already draining the broadcast before intake
  // (ClientIo, then the feed) can drive the engine to publish anything
  for (auto& slot : m_workers) {
    if (slot.has_value()) {
      slot->Start();
    }
  }
  m_io.Start();
  if (m_config.enable_feed) {
    m_feed.Start();
  }

  m_started = true;
  return true;
}

void Gateway::Stop() noexcept {
  if (!m_started) {
    return;
  }

  // Reverse of Start. Stop intake first (feed, then client I/O) so no new
  // commands enter and the engine can drain whatever is queued...
  if (m_config.enable_feed) {
    m_feed.Stop();
  }
  m_io.Stop();

  // ...then retire the workers, which leave the broadcast gracefully
  // (RetireOutputWorker) rather than being evicted for going silent
  for (auto& slot : m_workers) {
    if (slot.has_value()) {
      slot->Stop();
    }
  }

  m_started = false;
}

std::uint64_t Gateway::ClientDrops() const noexcept {
  std::uint64_t total{0};
  for (const auto& slot : m_workers) {
    if (slot.has_value()) {
      total += slot->Dropped();
    }
  }
  return total;
}

std::uint64_t Gateway::WorkerEvictions() const noexcept {
  std::uint64_t total{0};
  for (const auto& slot : m_workers) {
    if (slot.has_value()) {
      total += slot->Evictions();
    }
  }
  return total;
}

}  // namespace lfob
