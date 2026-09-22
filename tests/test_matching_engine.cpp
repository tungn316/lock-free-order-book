#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "lfob/execution_report.hpp"
#include "lfob/matching_engine.hpp"
#include "lfob/types.hpp"

using namespace lfob;

// ── helpers ──────────────────────────────────────────────────────────────────
namespace {

// Book bounds shared by most cases.
constexpr Price k_min_price = 1;
constexpr Price k_max_price = 1000;

// core = -1 => PinCurrentThread is a no-op that returns false, so the matching
// thread runs unpinned. Keeps these tests portable to CI / WSL.
constexpr int k_no_pin = -1;

// The same fluent command builder used in the order-book tests.
struct Cmd {
  OrderCommand c{};

  static Cmd New(OrderId id,
                 Side side,
                 Price price,
                 Quantity quantity,
                 TimeInForce tif = TimeInForce::DAY,
                 ClientId client = 1) {
    Cmd b;
    b.c.type = OrderCommand::Type::NEW;
    b.c.id = id;
    b.c.side = side;
    b.c.price = price;
    b.c.quantity = quantity;
    b.c.tif = tif;
    b.c.client = client;
    b.c.ingress_ts = 0;
    return b;
  }

  static Cmd Cancel(OrderId id, Side side = Side::BID, ClientId client = 1) {
    Cmd b;
    b.c.type = OrderCommand::Type::CANCEL;
    b.c.id = id;
    b.c.side = side;
    b.c.client = client;
    b.c.ingress_ts = 0;
    return b;
  }

  static Cmd Replace(OrderId id,
                     Side side,
                     Price price,
                     Quantity quantity,
                     TimeInForce tif = TimeInForce::DAY,
                     ClientId client = 1) {
    Cmd b;
    b.c.type = OrderCommand::Type::REPLACE;
    b.c.id = id;
    b.c.side = side;
    b.c.price = price;
    b.c.quantity = quantity;
    b.c.tif = tif;
    b.c.client = client;
    b.c.ingress_ts = 0;
    return b;
  }

  const OrderCommand& operator*() const noexcept { return c; }
};

// Spin-drain the egress ring for `id` until at least `want` reports have been
// collected or the deadline passes. Returns everything seen so far. The engine
// is asynchronous, so callers assert on the returned log rather than assuming
// a report is ready the instant Submit returns.
std::vector<ExecutionReport> Drain(
    MatchingEngine& eng,
    MatchingEngine::ConsumerId id,
    std::size_t want,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
  std::vector<ExecutionReport> log;
  std::array<ExecutionReport, 64> buf{};
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (log.size() < want && std::chrono::steady_clock::now() < deadline) {
    const std::size_t n = eng.ReadReports(id, buf.data(), buf.size());
    for (std::size_t i = 0; i < n; ++i) {
      log.push_back(buf.at(i));
    }
    if (n == 0) {
      std::this_thread::yield();
    }
  }
  return log;
}

// Count reports of a given type.
std::size_t Count(const std::vector<ExecutionReport>& log,
                  ExecutionReport::Type type) {
  std::size_t n = 0;
  for (const auto& r : log) {
    if (r.type == type) {
      ++n;
    }
  }
  return n;
}

// Last report of a given type, if any.
std::optional<ExecutionReport> Last(const std::vector<ExecutionReport>& log,
                                    ExecutionReport::Type type) {
  for (auto it = log.rbegin(); it != log.rend(); ++it) {
    if (it->type == type) {
      return *it;
    }
  }
  return std::nullopt;
}

// Spin until GetBbo reflects the expected bid, or the deadline passes.
Bbo WaitForBid(
    MatchingEngine& eng,
    Price want_bid,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  Bbo bbo = eng.GetBbo();
  while (bbo.bid_price != want_bid &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
    bbo = eng.GetBbo();
  }
  return bbo;
}

// Spin until GetBbo reflects the expected ask, or the deadline passes.
Bbo WaitForAsk(
    MatchingEngine& eng,
    Price want_ask,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  Bbo bbo = eng.GetBbo();
  while (bbo.ask_price != want_ask &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
    bbo = eng.GetBbo();
  }
  return bbo;
}

}  // namespace

// ── lifecycle ────────────────────────────────────────────────────────────────

TEST_CASE("engine constructs, starts, and stops cleanly",
          "[engine][lifecycle]") {
  // ~12 MB of inline ring buffers: too large for the thread stack, so it must
  // live on the heap (as it would in production). A stack local here overflows
  // and SIGSEGVs before Start() ever runs.
  auto eng_ptr =
      std::make_unique<MatchingEngine>(k_min_price, k_max_price, k_no_pin);
  auto& eng = *eng_ptr;
  eng.Start();
  eng.Stop();
  SUCCEED("no crash or hang across start/stop");
}

TEST_CASE("destructor stops the matching thread without an explicit Stop",
          "[engine][lifecycle]") {
  {
    // ~12 MB of inline ring buffers: too large for the thread stack, so it must
    // live on the heap (as it would in production). A stack local here
    // overflows and SIGSEGVs before Start() ever runs.
    auto eng_ptr =
        std::make_unique<MatchingEngine>(k_min_price, k_max_price, k_no_pin);
    auto& eng = *eng_ptr;
    eng.Start();
    // Leaving scope must join the thread via the destructor.
  }
  SUCCEED("destructor joined cleanly");
}

TEST_CASE("Stop is idempotent", "[engine][lifecycle]") {
  // ~12 MB of inline ring buffers: too large for the thread stack, so it must
  // live on the heap (as it would in production). A stack local here overflows
  // and SIGSEGVs before Start() ever runs.
  auto eng_ptr =
      std::make_unique<MatchingEngine>(k_min_price, k_max_price, k_no_pin);
  auto& eng = *eng_ptr;
  eng.Start();
  eng.Stop();
  eng.Stop();  // second Stop must be a no-op, not a double-join crash
  SUCCEED("second Stop is harmless");
}

// ── ingress → book → egress pipeline ─────────────────────────────────────────

TEST_CASE("a resting order flows through to a TopOfBook report",
          "[engine][pipeline]") {
  // ~12 MB of inline ring buffers: too large for the thread stack, so it must
  // live on the heap (as it would in production). A stack local here overflows
  // and SIGSEGVs before Start() ever runs.
  auto eng_ptr =
      std::make_unique<MatchingEngine>(k_min_price, k_max_price, k_no_pin);
  auto& eng = *eng_ptr;
  const auto id = eng.RegisterOutputWorker();
  eng.Start();

  REQUIRE(eng.Submit(*Cmd::New(1, Side::BID, 100, 10)));

  const auto log = Drain(eng, id, /*want=*/2);
  CHECK(Count(log, ExecutionReport::Type::ACCEPTED) == 1);

  const auto tob = Last(log, ExecutionReport::Type::TOP_OF_BOOK);
  REQUIRE(tob.has_value());
  CHECK(tob->bid_price == 100);
  CHECK(tob->bid_quantity == 10);
}

TEST_CASE("GetBbo publishes the top of book to reader threads",
          "[engine][bbo]") {
  // ~12 MB of inline ring buffers: too large for the thread stack, so it must
  // live on the heap (as it would in production). A stack local here overflows
  // and SIGSEGVs before Start() ever runs.
  auto eng_ptr =
      std::make_unique<MatchingEngine>(k_min_price, k_max_price, k_no_pin);
  auto& eng = *eng_ptr;
  eng.Start();

  REQUIRE(eng.Submit(*Cmd::New(1, Side::BID, 100, 10)));
  REQUIRE(eng.Submit(*Cmd::New(2, Side::ASK, 105, 7)));

  // The ask is submitted second and the matching thread drains ingress in
  // FIFO order, so an ask quote in the BBO implies the earlier bid landed too.
  // Waiting on the bid alone would race: bid_price flips to 100 the moment
  // order 1 is applied, often before order 2's ask reaches the seqlock.
  const Bbo bbo = WaitForAsk(eng, 105);
  CHECK(bbo.bid_price == 100);
  CHECK(bbo.bid_quantity == 10);
  CHECK(bbo.ask_price == 105);
  CHECK(bbo.ask_quantity == 7);
}

TEST_CASE("SubmitBulk pushes every command through the pipeline",
          "[engine][pipeline]") {
  // ~12 MB of inline ring buffers: too large for the thread stack, so it must
  // live on the heap (as it would in production). A stack local here overflows
  // and SIGSEGVs before Start() ever runs.
  auto eng_ptr =
      std::make_unique<MatchingEngine>(k_min_price, k_max_price, k_no_pin);
  auto& eng = *eng_ptr;
  const auto id = eng.RegisterOutputWorker();
  eng.Start();

  const std::array<OrderCommand, 3> cmds{
      *Cmd::New(1, Side::BID, 100, 10),
      *Cmd::New(2, Side::BID, 101, 10),
      *Cmd::New(3, Side::BID, 102, 10),
  };
  REQUIRE(eng.SubmitBulk(cmds.data(), cmds.size()) == cmds.size());

  // Each resting order emits two reports: ACCEPTED + TOP_OF_BOOK. Drain counts
  // total reports, so wait for all 6 before asserting on the 3 ACCEPTEDs.
  const auto log = Drain(eng, id, /*want=*/6);
  CHECK(Count(log, ExecutionReport::Type::ACCEPTED) == 3);

  const Bbo bbo = WaitForBid(eng, 102);
  CHECK(bbo.bid_price == 102);
}

// ── matching across the boundary ─────────────────────────────────────────────

TEST_CASE("aggressive order matches a resting order and emits fills",
          "[engine][match]") {
  // ~12 MB of inline ring buffers: too large for the thread stack, so it must
  // live on the heap (as it would in production). A stack local here overflows
  // and SIGSEGVs before Start() ever runs.
  auto eng_ptr =
      std::make_unique<MatchingEngine>(k_min_price, k_max_price, k_no_pin);
  auto& eng = *eng_ptr;
  const auto id = eng.RegisterOutputWorker();
  eng.Start();

  REQUIRE(eng.Submit(*Cmd::New(1, Side::ASK, 100, 10)));  // maker
  REQUIRE(eng.Submit(*Cmd::New(2, Side::BID, 100, 10)));  // taker crosses

  // Wait for the two fills (maker + taker side) plus surrounding reports.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  std::vector<ExecutionReport> log;
  std::array<ExecutionReport, 64> buf{};
  while (Count(log, ExecutionReport::Type::FILL) < 2 &&
         std::chrono::steady_clock::now() < deadline) {
    const std::size_t n = eng.ReadReports(id, buf.data(), buf.size());
    for (std::size_t i = 0; i < n; ++i) {
      log.push_back(buf.at(i));
    }
    if (n == 0) {
      std::this_thread::yield();
    }
  }

  CHECK(Count(log, ExecutionReport::Type::FILL) == 2);
}

TEST_CASE("cancel flows through and clears the book", "[engine][cancel]") {
  // ~12 MB of inline ring buffers: too large for the thread stack, so it must
  // live on the heap (as it would in production). A stack local here overflows
  // and SIGSEGVs before Start() ever runs.
  auto eng_ptr =
      std::make_unique<MatchingEngine>(k_min_price, k_max_price, k_no_pin);
  auto& eng = *eng_ptr;
  const auto id = eng.RegisterOutputWorker();
  eng.Start();

  REQUIRE(eng.Submit(*Cmd::New(1, Side::BID, 100, 10)));
  WaitForBid(eng, 100);
  REQUIRE(eng.Submit(*Cmd::Cancel(1, Side::BID)));

  // Poll until the book empties out again.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  Bbo bbo = eng.GetBbo();
  while (bbo.bid_price != k_no_price &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
    bbo = eng.GetBbo();
  }
  CHECK(bbo.bid_price == k_no_price);

  const auto log = Drain(eng, id, /*want=*/3);
  CHECK(Count(log, ExecutionReport::Type::CANCELLED) == 1);
}

TEST_CASE("an invalid command surfaces a REJECTED report", "[engine][reject]") {
  // ~12 MB of inline ring buffers: too large for the thread stack, so it must
  // live on the heap (as it would in production). A stack local here overflows
  // and SIGSEGVs before Start() ever runs.
  auto eng_ptr =
      std::make_unique<MatchingEngine>(k_min_price, k_max_price, k_no_pin);
  auto& eng = *eng_ptr;
  const auto id = eng.RegisterOutputWorker();
  eng.Start();

  REQUIRE(eng.Submit(*Cmd::New(1, Side::BID, 100, 0)));  // zero quantity

  const auto log = Drain(eng, id, /*want=*/1);
  const auto rej = Last(log, ExecutionReport::Type::REJECTED);
  REQUIRE(rej.has_value());
  CHECK(rej->reason == ExecutionReport::RejectReason::ZERO_QUANTITY);
}

// ── egress broadcast: every registered consumer sees every report ────────────

TEST_CASE("two output workers each receive the full report stream",
          "[engine][egress][broadcast]") {
  // ~12 MB of inline ring buffers: too large for the thread stack, so it must
  // live on the heap (as it would in production). A stack local here overflows
  // and SIGSEGVs before Start() ever runs.
  auto eng_ptr =
      std::make_unique<MatchingEngine>(k_min_price, k_max_price, k_no_pin);
  auto& eng = *eng_ptr;
  const auto a = eng.RegisterOutputWorker();
  const auto b = eng.RegisterOutputWorker();
  eng.Start();

  REQUIRE(eng.Submit(*Cmd::New(1, Side::BID, 100, 10)));

  const auto log_a = Drain(eng, a, /*want=*/2);
  const auto log_b = Drain(eng, b, /*want=*/2);

  CHECK(Count(log_a, ExecutionReport::Type::ACCEPTED) == 1);
  CHECK(Count(log_b, ExecutionReport::Type::ACCEPTED) == 1);
  CHECK(Count(log_a, ExecutionReport::Type::TOP_OF_BOOK) ==
        Count(log_b, ExecutionReport::Type::TOP_OF_BOOK));
}

// ── shutdown drains accepted-but-unprocessed commands ────────────────────────

TEST_CASE("Stop drains commands still queued at shutdown",
          "[engine][lifecycle][drain]") {
  // ~12 MB of inline ring buffers: too large for the thread stack, so it must
  // live on the heap (as it would in production). A stack local here overflows
  // and SIGSEGVs before Start() ever runs.
  auto eng_ptr =
      std::make_unique<MatchingEngine>(k_min_price, k_max_price, k_no_pin);
  auto& eng = *eng_ptr;
  const auto id = eng.RegisterOutputWorker();
  eng.Start();

  constexpr std::size_t k_n = 200;
  for (OrderId i = 1; i <= k_n; ++i) {
    // Distinct prices inside the band so each rests (no self-cross).
    REQUIRE(eng.Submit(*Cmd::New(i, Side::BID, 100 + (i % 300), 1)));
  }

  // Stop must poll the ingress dry before joining, so every accepted command
  // has produced at least its ACCEPTED report by the time Stop returns.
  eng.Stop();

  // Two reports per resting order (ACCEPTED + TOP_OF_BOOK). Drain counts total
  // reports, so target 2 * k_n; the ACCEPTED subset must still be exactly k_n.
  const auto log = Drain(eng, id, /*want=*/2 * k_n);
  CHECK(Count(log, ExecutionReport::Type::ACCEPTED) == k_n);
}

// ── a dead output worker must not be able to stall the engine ────────────────

TEST_CASE("a wedged output worker is evicted instead of stalling the book",
          "[engine][egress][evict]") {
  auto eng_ptr =
      std::make_unique<MatchingEngine>(k_min_price, k_max_price, k_no_pin);
  auto& eng = *eng_ptr;

  const auto live = eng.RegisterOutputWorker();
  const auto dead = eng.RegisterOutputWorker();  // registers, never reads
  eng.Start();

  // Enough resting orders to overflow the egress ring: each one emits an
  // ACCEPTED and a TOP_OF_BOOK (the level's quantity changes every time), so
  // ~2x this many reports against a 65536-slot ring.
  constexpr std::size_t k_orders = 40'000;

  std::atomic<std::size_t> accepted{0};
  std::atomic<bool> stop{false};
  std::atomic<bool> live_evicted{false};

  std::thread drainer([&] {
    std::array<ExecutionReport, 256> buf{};
    while (!stop.load(std::memory_order::relaxed)) {
      const std::size_t n = eng.ReadReports(live, buf.data(), buf.size());
      for (std::size_t i = 0; i < n; ++i) {
        if (buf.at(i).type == ExecutionReport::Type::ACCEPTED) {
          accepted.fetch_add(1, std::memory_order::relaxed);
        }
      }
      // What a real output worker does when it comes up empty: an empty read
      // is indistinguishable from a quiet market, so ask. On an unpinned CI
      // box this worker can itself be starved past k_egress_stall_budget_ns,
      // and then the honest answer is a gap, not a longer wait.
      if (n == 0 && !eng.IsOutputWorkerLive(live)) {
        live_evicted.store(true);
        eng.RejoinOutputWorker(live);
      }
    }
  });

  // Without eviction this loop never finishes: the matching thread wedges on
  // the full egress ring, ingress backs up and every Submit starts failing.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  std::size_t sent = 0;
  while (sent < k_orders && std::chrono::steady_clock::now() < deadline) {
    if (eng.Submit(
            *Cmd::New(static_cast<OrderId>(sent + 1), Side::ASK, 500, 1))) {
      ++sent;
    }
  }

  while (accepted.load() < k_orders && !live_evicted.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  stop.store(true);
  drainer.join();

  // The claim under test: the engine kept accepting orders. Without eviction
  // ingress would have filled and Submit would have started failing.
  CHECK(sent == k_orders);

  // The worker that stopped draining was thrown overboard.
  CHECK(eng.EgressEvictions() >= 1);
  CHECK_FALSE(eng.IsOutputWorkerLive(dead));

  // Unless the scheduler starved the healthy worker past the budget as well,
  // it stayed attached and missed nothing.
  if (!live_evicted.load()) {
    CHECK(accepted.load() == k_orders);
    CHECK(eng.IsOutputWorkerLive(live));
  }

  // Recovery is a rejoin at the live edge, not a replay of the backlog.
  REQUIRE(eng.RejoinOutputWorker(dead));
  CHECK(eng.IsOutputWorkerLive(dead));

  std::array<ExecutionReport, 8> buf{};
  CHECK(eng.ReadReports(dead, buf.data(), buf.size()) == 0);

  // The BBO seqlock is readable throughout, which is what lets a rejoining
  // worker resync its clients without touching the ring it was evicted from.
  CHECK(eng.GetBbo().ask_price == 500);
}

// ── global sequencing across the whole stream ────────────────────────────────

TEST_CASE("egress reports carry strictly increasing seq numbers",
          "[engine][egress][seq]") {
  // ~12 MB of inline ring buffers: too large for the thread stack, so it must
  // live on the heap (as it would in production). A stack local here overflows
  // and SIGSEGVs before Start() ever runs.
  auto eng_ptr =
      std::make_unique<MatchingEngine>(k_min_price, k_max_price, k_no_pin);
  auto& eng = *eng_ptr;
  const auto id = eng.RegisterOutputWorker();
  eng.Start();

  REQUIRE(eng.Submit(*Cmd::New(1, Side::ASK, 100, 5)));
  REQUIRE(eng.Submit(*Cmd::New(2, Side::ASK, 101, 5)));
  REQUIRE(eng.Submit(*Cmd::New(3, Side::BID, 101, 10)));  // sweeps both

  const auto log = Drain(eng, id, /*want=*/6);
  REQUIRE(log.size() >= 2);

  SeqNum prev = log.front().seq;
  for (std::size_t i = 1; i < log.size(); ++i) {
    CHECK(log.at(i).seq > prev);  // strictly increasing, no reordering
    prev = log.at(i).seq;
  }
}
