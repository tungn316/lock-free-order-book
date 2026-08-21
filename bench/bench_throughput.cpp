// bench_throughput — steady-state ingress→match→egress throughput for the
// matching engine, driven directly through its public ring API. No gateway,
// no sockets: this measures the book and matching thread, not the network.
//
// Workload: N resting NEW bids spread across a price band. All bids, no asks,
// so nothing ever crosses — every command takes the pure insertion path and
// rests. N is kept at/below the node arena capacity so the run does not
// degrade into ARENA_EXHAUSTED rejects.
//
// Because Emit() back-pressures losslessly (it spins until the slowest egress
// consumer makes room), a benchmark that does not drain egress would wedge the
// matching thread the instant the SPMC ring fills. So a background consumer
// thread drains reports continuously for the whole run, including through
// Stop()'s shutdown drain.
//
// Usage: bench_throughput [num_orders] [cpu_core]
//   num_orders  total NEW commands to push        (default 1'000'000)
//   cpu_core    core to pin the matching thread to (default -1 = no pin)

#include <lfob/clock.hpp>
#include <lfob/execution_report.hpp>
#include <lfob/matching_engine.hpp>
#include <lfob/types.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace lfob;

// Price band the resting bids cycle through. Narrow enough that many orders
// share a level (exercising FIFO append at a price), wide enough to move the
// top of book. Must sit inside [k_min_price, k_max_price].
constexpr Price k_min_price = 1;
constexpr Price k_max_price = 4096;

// Pushed to the engine in one contiguous slice per call; the engine stages it
// through its own bounded buffer, so a large batch here is fine.
constexpr std::size_t k_push_chunk = 4096;

// Parse an unsigned/int argv token, falling back to `fallback` on anything
// unparseable so a stray argument never silently runs a garbage config.
template <typename T>
T ParseArg(int argc, char** argv, int index, T fallback) {
  if (index >= argc) {
    return fallback;
  }
  const std::string_view tok{argv[index]};
  T value{};
  const auto* end = tok.data() + tok.size();
  const auto [ptr, ec] = std::from_chars(tok.data(), end, value);
  if (ec != std::errc{} || ptr != end) {
    return fallback;
  }
  return value;
}

// Build the command stream once, up front, so the timed region measures the
// engine and not RNG or allocation. All NEW bids, prices cycling across the
// band, ids strictly increasing.
std::vector<OrderCommand> BuildWorkload(std::size_t count) {
  std::vector<OrderCommand> cmds;
  cmds.reserve(count);
  const Price span = k_max_price - k_min_price + 1;
  for (std::size_t i = 0; i < count; ++i) {
    OrderCommand c{};
    c.type = OrderCommand::Type::NEW;
    c.side = Side::BID;
    c.tif = TimeInForce::DAY;
    c.client = 1;
    c.id = static_cast<OrderId>(i + 1);
    c.price = k_min_price + static_cast<Price>(i % span);
    c.quantity = 1;
    c.ingress_ts = 0;  // Submit path restamps this.
    cmds.push_back(c);
  }
  return cmds;
}

// Push every command, spinning on back-pressure. SubmitBulk returns a short
// count when the ingress ring is full; we retry the remainder rather than drop,
// so the engine sees all N commands and throughput reflects sustained draining.
void SubmitAll(MatchingEngine& eng, std::span<const OrderCommand> cmds) {
  std::size_t sent = 0;
  while (sent < cmds.size()) {
    const std::size_t want =
        std::min(k_push_chunk, cmds.size() - sent);
    const std::size_t n = eng.SubmitBulk(cmds.data() + sent, want);
    sent += n;
    if (n < want) {
      std::this_thread::yield();  // ingress full: let the matching thread catch up
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t num_orders =
      ParseArg<std::size_t>(argc, argv, 1, 1'000'000);
  const int cpu_core = ParseArg<int>(argc, argv, 2, -1);

  if (num_orders > k_max_orders) {
    std::fprintf(stderr,
                 "num_orders %zu exceeds arena capacity %zu; capping\n",
                 num_orders, k_max_orders);
  }
  const std::size_t n = std::min(num_orders, k_max_orders);

  const std::vector<OrderCommand> cmds = BuildWorkload(n);

  // ~12 MB of inline ring buffers — too big for a stack local, must be heap.
  auto eng = std::make_unique<MatchingEngine>(k_min_price, k_max_price,
                                              cpu_core);
  const auto consumer = eng->RegisterOutputWorker();

  // Drain egress for the whole run so Emit() never wedges on a full ring.
  // Stops only after main flips the flag, which happens after Stop() returns
  // and the matching thread has emitted its last report.
  std::atomic<bool> draining{true};
  std::atomic<std::uint64_t> reports_seen{0};
  std::thread drainer([&] {
    std::vector<ExecutionReport> buf(k_drain_batch);
    std::uint64_t local = 0;
    while (draining.load(std::memory_order::relaxed)) {
      const std::size_t got =
          eng->ReadReports(consumer, buf.data(), buf.size());
      if (got == 0) {
        std::this_thread::yield();
      } else {
        local += got;
      }
    }
    // Final sweep: pick up anything emitted during Stop()'s drain.
    for (std::size_t got = eng->ReadReports(consumer, buf.data(), buf.size());
         got > 0;
         got = eng->ReadReports(consumer, buf.data(), buf.size())) {
      local += got;
    }
    reports_seen.store(local, std::memory_order::relaxed);
  });

  eng->Start();

  const Timestamp t0 = NowNanos();
  SubmitAll(*eng, cmds);
  // Stop() polls the ingress ring dry and joins the matching thread, so once it
  // returns every accepted command has been applied and reported.
  eng->Stop();
  const Timestamp t1 = NowNanos();

  draining.store(false, std::memory_order::relaxed);
  drainer.join();

  const double elapsed_s = static_cast<double>(t1 - t0) / 1e9;
  const double throughput = static_cast<double>(n) / elapsed_s;
  const double ns_per_order =
      static_cast<double>(t1 - t0) / static_cast<double>(n);

  std::printf("orders           : %zu\n", n);
  std::printf("reports drained  : %llu\n",
              static_cast<unsigned long long>(
                  reports_seen.load(std::memory_order::relaxed)));
  std::printf("elapsed          : %.3f ms\n", elapsed_s * 1e3);
  std::printf("throughput       : %.2f M orders/s\n", throughput / 1e6);
  std::printf("latency (amort.) : %.1f ns/order\n", ns_per_order);
  std::printf("matching core    : %d\n", cpu_core);

  return 0;
}
