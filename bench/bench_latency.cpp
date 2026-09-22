// bench_latency — ingress→match service-latency distribution for the matching
// engine, driven directly through its ring API. No gateway, no sockets.
//
// Each ExecutionReport carries two stamps on one monotonic clock: ingress_ts,
// set by Submit() when the command enters the ingress ring, and match_ts, set
// by Emit() when the matching thread finishes producing the report. Their
// difference is the per-event latency — time spent in the ingress ring plus the
// book work — with no cross-thread clock skew, since both stamps come from the
// same NowNanos().
//
// Latency, unlike throughput, must not be measured under a flood: saturating
// the ingress ring turns the number into a measure of queue depth, not engine
// speed. So submissions are PACED to a target offered rate that keeps the queue
// near-empty, exposing service latency rather than queueing latency. Push the
// rate up toward the throughput ceiling to watch the tail grow.
//
// Usage: bench_latency [num_samples] [rate_per_sec] [cpu_core] [bench_core]
//   num_samples   NEW commands to measure          (default 500'000)
//   rate_per_sec  offered load, orders/sec         (default 500'000)
//   cpu_core      core to pin the matching thread  (default -1 = no pin)
//   bench_core    core to pin the submitter and drainer threads to
//                 (default -1 = no pin). Matters under isolcpus: an
//                 unpinned thread that happens to land on cpu_core at
//                 creation sticks there for the whole run.

#include <lfob/affinity.hpp>
#include <lfob/clock.hpp>
#include <lfob/execution_report.hpp>
#include <lfob/matching_engine.hpp>
#include <lfob/types.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace lfob;

constexpr Price k_min_price = 1;
constexpr Price k_max_price = 4096;

// Untimed orders pushed before sampling begins, to warm caches / branch
// predictors and settle the drainer before any latency is recorded
constexpr std::size_t k_warmup = 20'000;

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

OrderCommand MakeBid(OrderId id) {
  OrderCommand c{};
  c.type = OrderCommand::Type::NEW;
  c.side = Side::BID;
  c.tif = TimeInForce::DAY;
  c.client = 1;
  c.id = id;
  const Price span = k_max_price - k_min_price + 1;
  c.price = k_min_price + static_cast<Price>(id % span);
  c.quantity = 1;
  c.ingress_ts = 0;  // Submit restamps this.
  return c;
}

// Nearest-rank percentile over an already-sorted ascending vector.
std::uint64_t Percentile(const std::vector<std::uint64_t>& sorted, double p) {
  if (sorted.empty()) {
    return 0;
  }
  const double rank = p * static_cast<double>(sorted.size() - 1);
  const auto idx = static_cast<std::size_t>(rank + 0.5);
  return sorted[std::min(idx, sorted.size() - 1)];
}

// Fixed log-ish bucket edges in nanoseconds. Anything past the last edge lands
// in the overflow row.
constexpr std::array<std::uint64_t, 9> k_edges_ns{
    250, 500, 1'000, 2'000, 5'000, 10'000, 25'000, 50'000, 100'000};

void PrintHistogram(const std::vector<std::uint64_t>& samples) {
  std::array<std::size_t, k_edges_ns.size() + 1> counts{};
  for (const std::uint64_t v : samples) {
    std::size_t b = 0;
    while (b < k_edges_ns.size() && v >= k_edges_ns[b]) {
      ++b;
    }
    ++counts[b];
  }

  std::size_t max_count = 1;
  for (const std::size_t c : counts) {
    max_count = std::max(max_count, c);
  }

  constexpr int k_bar_width = 40;
  std::printf("\nlatency histogram (match_ts - ingress_ts)\n");
  for (std::size_t b = 0; b < counts.size(); ++b) {
    char label[32];
    if (b == 0) {
      std::snprintf(label, sizeof(label), "      < %6llu ns",
                    static_cast<unsigned long long>(k_edges_ns[0]));
    } else if (b == k_edges_ns.size()) {
      std::snprintf(
          label, sizeof(label), ">= %6llu ns      ",
          static_cast<unsigned long long>(k_edges_ns[k_edges_ns.size() - 1]));
    } else {
      std::snprintf(label, sizeof(label), "%6llu - %6llu ns",
                    static_cast<unsigned long long>(k_edges_ns[b - 1]),
                    static_cast<unsigned long long>(k_edges_ns[b]));
    }

    const int bar = static_cast<int>(
        (static_cast<std::uint64_t>(counts[b]) * k_bar_width) / max_count);
    char bars[k_bar_width + 1];
    for (int i = 0; i < bar; ++i) {
      bars[i] = '#';
    }
    bars[bar] = '\0';

    const double pct = samples.empty()
                           ? 0.0
                           : 100.0 * static_cast<double>(counts[b]) /
                                 static_cast<double>(samples.size());
    std::printf("  %-18s | %-40s %8zu (%5.1f%%)\n", label, bars, counts[b],
                pct);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::size_t num_samples = ParseArg<std::size_t>(argc, argv, 1, 500'000);
  const std::uint64_t rate = ParseArg<std::uint64_t>(argc, argv, 2, 500'000);
  const int cpu_core = ParseArg<int>(argc, argv, 3, -1);
  // Core for the submitter (this thread) and the drainer thread. Left
  // unpinned (-1) by default, both float across whatever mask the caller
  // (e.g. taskset) set. Under isolcpus, an unpinned thread that happens to
  // get initially placed on cpu_core shares it with the matching thread for
  // the rest of the run -- isolated cores are excluded from the normal
  // load-balancing domain, so nothing ever moves it back off. Pinning here
  // makes sure that never happens.
  const int bench_core = ParseArg<int>(argc, argv, 4, -1);
  if (bench_core >= 0) {
    PinCurrentThread(bench_core);
  }

  const std::size_t total = std::min(num_samples + k_warmup, k_max_orders);
  const std::size_t samples_wanted = total > k_warmup ? total - k_warmup : 0;

  // Inter-arrival gap in ns for the target offered rate. 0 => submit as fast as
  // possible (open the floodgates to see the saturated tail).
  const std::uint64_t gap_ns = rate == 0 ? 0 : 1'000'000'000ULL / rate;

  auto eng =
      std::make_unique<MatchingEngine>(k_min_price, k_max_price, cpu_core);
  const auto consumer = eng->RegisterOutputWorker();

  // Drainer collects one latency sample per ACCEPTED report — exactly one per
  // NEW command, so TOP_OF_BOOK/etc. never double-count. Warmup ids (<=
  // k_warmup) are drained but not recorded. The vector is reserved up front,
  // and storing into it does not affect the measurement: each delta is already
  // fixed by the two timestamps the engine stamped long before the drainer sees
  // the report.
  std::atomic<bool> draining{true};
  std::vector<std::uint64_t> samples;
  samples.reserve(samples_wanted);
  std::thread drainer([&] {
    if (bench_core >= 0) {
      PinCurrentThread(bench_core);
    }
    std::vector<ExecutionReport> buf(k_drain_batch);
    auto collect = [&](std::size_t got) {
      for (std::size_t i = 0; i < got; ++i) {
        const ExecutionReport& r = buf[i];
        if (r.type == ExecutionReport::Type::ACCEPTED &&
            r.order_id > k_warmup) {
          samples.push_back(r.match_ts - r.ingress_ts);
        }
      }
    };
    while (draining.load(std::memory_order::relaxed)) {
      const std::size_t got =
          eng->ReadReports(consumer, buf.data(), buf.size());
      if (got == 0) {
        std::this_thread::yield();
      } else {
        collect(got);
      }
    }
    for (std::size_t got = eng->ReadReports(consumer, buf.data(), buf.size());
         got > 0; got = eng->ReadReports(consumer, buf.data(), buf.size())) {
      collect(got);
    }
  });

  eng->Start();

  // Paced submission. Busy-wait to the next send time so pacing stays tight
  // even at high rates; a sleep would jitter far more than the latencies we
  // measure. On back-pressure (should not happen below saturation) retry.
  Timestamp next = NowNanos();
  for (OrderId id = 1; id <= total; ++id) {
    if (gap_ns != 0) {
      while (NowNanos() < next) {
        // spin to target inter-arrival time
      }
      next += gap_ns;
    }
    const OrderCommand cmd = MakeBid(id);
    while (!eng->Submit(cmd)) {
      std::this_thread::yield();
    }
  }

  eng->Stop();  // drains ingress + joins matching thread
  draining.store(false, std::memory_order::relaxed);
  drainer.join();

  if (samples.empty()) {
    std::fprintf(stderr, "no samples collected\n");
    return 1;
  }

  std::sort(samples.begin(), samples.end());

  std::uint64_t sum = 0;
  for (const std::uint64_t v : samples) {
    sum += v;
  }
  const double mean =
      static_cast<double>(sum) / static_cast<double>(samples.size());

  std::printf("samples          : %zu\n", samples.size());
  std::printf("offered rate     : %llu orders/s%s\n",
              static_cast<unsigned long long>(rate),
              gap_ns == 0 ? " (unpaced / saturated)" : "");
  std::printf("matching core    : %d\n", cpu_core);
  std::printf("\nlatency ns (match_ts - ingress_ts)\n");
  std::printf("  min            : %llu\n",
              static_cast<unsigned long long>(samples.front()));
  std::printf("  mean           : %.0f\n", mean);
  std::printf("  p50            : %llu\n",
              static_cast<unsigned long long>(Percentile(samples, 0.50)));
  std::printf("  p90            : %llu\n",
              static_cast<unsigned long long>(Percentile(samples, 0.90)));
  std::printf("  p99            : %llu\n",
              static_cast<unsigned long long>(Percentile(samples, 0.99)));
  std::printf("  p99.9          : %llu\n",
              static_cast<unsigned long long>(Percentile(samples, 0.999)));
  std::printf("  p99.99         : %llu\n",
              static_cast<unsigned long long>(Percentile(samples, 0.9999)));
  std::printf("  max            : %llu\n",
              static_cast<unsigned long long>(samples.back()));

  PrintHistogram(samples);
  return 0;
}
