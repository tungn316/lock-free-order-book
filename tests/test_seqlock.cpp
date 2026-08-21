#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <numeric>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "lfob/seqlock.hpp"
#include "lfob/types.hpp"

using namespace lfob;
using namespace std::chrono_literals;

// ── helpers ──────────────────────────────────────────────────────────────────
namespace {

// Compiler-opaque spin. Prevents the writer from finishing its burst before
// readers get scheduled, without a syscall.
inline void SpinPause(int iters) noexcept {
  for (int i = 0; i < iters; ++i) {
    std::atomic_signal_fence(std::memory_order::seq_cst);
  }
}

inline Bbo MakeBbo(SeqNum seq, Price bp, Quantity bq, Price ap, Quantity aq) {
  return {.event_seq = seq,
          .bid_price = bp,
          .bid_quantity = bq,
          .ask_price = ap,
          .ask_quantity = aq};
}

inline bool BboEq(const Bbo& a, const Bbo& b) {
  return a.event_seq == b.event_seq && a.bid_price == b.bid_price &&
         a.bid_quantity == b.bid_quantity && a.ask_price == b.ask_price &&
         a.ask_quantity == b.ask_quantity;
}

}  // namespace

// ── single-threaded
// ───────────────────────────────────────────────────────────

TEST_CASE("default-constructed load returns zero", "[seqlock][single]") {
  const Seqlock<Bbo> sl;
  const Bbo val = sl.Load();
  CHECK(val.event_seq == 0);
  CHECK(val.bid_price == 0);
  CHECK(val.bid_quantity == 0);
  CHECK(val.ask_price == 0);
  CHECK(val.ask_quantity == 0);
}

TEST_CASE("Store then Load roundtrips correctly", "[seqlock][single]") {
  Seqlock<Bbo> sl;
  const Bbo written = MakeBbo(42, 100, 10, 101, 5);
  sl.Store(written);
  CHECK(BboEq(sl.Load(), written));
}

TEST_CASE("multiple sequential stores: last write wins", "[seqlock][single]") {
  Seqlock<Bbo> sl;
  for (long i = 0; i < 1'000; ++i) {
    sl.Store(MakeBbo(i, i * 10, i * 2, (i * 10) + 1, i * 3));
  }
  const Bbo val = sl.Load();
  CHECK(val.event_seq == 999);
  CHECK(val.bid_price == 9990);
  CHECK(val.ask_price == 9991);
}

TEST_CASE("works with trivial scalar type", "[seqlock][scalar]") {
  Seqlock<int> sl;
  sl.Store(42);
  CHECK(sl.Load() == 42);
}

// ── concurrent
// ────────────────────────────────────────────────────────────────

// Invariant baked into every write: ask_price == bid_price + 1.
// Any torn read will break this, giving us a deterministic signal.
TEST_CASE("readers never observe a torn write", "[seqlock][concurrent]") {
  Seqlock<Bbo> sl;

  constexpr int k_writes = 50'000;
  constexpr int k_readers = 4;
  constexpr int k_writer_gap = 200;

  sl.Store(MakeBbo(0, 0, 1, 1, 1));

  std::atomic<bool> writing{true};

  std::vector<int> max_id(k_readers, -1);
  std::vector<int> torn(k_readers, 0);

  std::barrier sync(k_readers + 1);

  std::thread writer([&] {
    sync.arrive_and_wait();
    for (int i{1}; i <= k_writes; ++i) {
      const auto p = static_cast<Price>(i);
      sl.Store(MakeBbo(i, p, 1, p + 1, 1));
      SpinPause(k_writer_gap);
    }
    writing.store(false, std::memory_order::release);
  });

  std::vector<std::jthread> readers;
  readers.reserve(k_readers);
  for (int r = 0; r < k_readers; ++r) {
    readers.emplace_back([&, r] {
      sync.arrive_and_wait();
      int local_torn{0};
      int local_max{-1};
      while (writing.load(std::memory_order::acquire)) {
        const Bbo v = sl.Load();
        if (v.ask_price != v.bid_price + 1) {
          ++local_torn;
        }
        local_max = std::max(local_max, static_cast<int>(v.event_seq));
      }
      torn[r] = local_torn;
      max_id[r] = local_max;
    });
  }

  writer.join();
  for (auto& t : readers) {
    t.join();
  }
  readers.clear();

  const int total_torn = std::reduce(torn.begin(), torn.end());
  const int highest_id = *std::ranges::max_element(max_id);

  CHECK(total_torn == 0);
  CHECK(highest_id > 0);
  CHECK(highest_id <= k_writes);
}

TEST_CASE("single writer, single reader: final value consistent",
          "[seqlock][concurrent]") {
  Seqlock<Bbo> sl;
  constexpr int k_writes = 100'000;

  std::atomic<bool> start{false};
  Price final_price{};

  std::thread writer([&] {
    while (!start.load(std::memory_order_relaxed)) {
    }
    for (long i = 0; i < k_writes; ++i) {
      sl.Store(MakeBbo(i, i, 1, i + 1, 1));
    }
    final_price = static_cast<Price>(k_writes - 1);
  });

  start.store(true, std::memory_order_release);
  writer.join();

  const Bbo val = sl.Load();
  CHECK(val.bid_price == final_price);
  CHECK(val.ask_price == final_price + 1);
}
