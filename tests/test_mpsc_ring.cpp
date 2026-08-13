#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <numeric>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "lfob/mpsc_ring.hpp"
#include "lfob/types.hpp"

using namespace lfob;

// ── single-threaded
// ───────────────────────────────────────────────────────────

TEST_CASE("empty ring pops nullopt", "[mpsc][single]") {
  MpscRing<int, 4> q;
  CHECK_FALSE(q.TryPop().has_value());
  CHECK(q.SizeApprox() == 0);
}

TEST_CASE("push then pop roundtrips FIFO", "[mpsc][single]") {
  MpscRing<int, 8> q;
  for (int i = 0; i < 8; ++i) {
    CHECK(q.TryPush(i));
  }
  for (int i = 0; i < 8; ++i) {
    auto v = q.TryPop();
    REQUIRE(v.has_value());
    CHECK(*v == i);
  }
  CHECK_FALSE(q.TryPop().has_value());
}

TEST_CASE("push fails when full", "[mpsc][single]") {
  MpscRing<int, 4> q;
  for (int i = 0; i < 4; ++i) {
    CHECK(q.TryPush(i));
  }
  CHECK_FALSE(q.TryPush(99));  // full
  CHECK(q.SizeApprox() == 4);

  auto v = q.TryPop();
  REQUIRE(v.has_value());
  CHECK(*v == 0);
  CHECK(q.TryPush(99));  // room now
}

TEST_CASE("wraparound preserves FIFO order", "[mpsc][single]") {
  MpscRing<int, 4> q;
  int next_push = 0;
  int next_pop = 0;
  for (int round = 0; round < 1000; ++round) {
    while (q.TryPush(next_push)) {
      ++next_push;
    }
    auto v = q.TryPop();
    REQUIRE(v.has_value());
    CHECK(*v == next_pop);
    ++next_pop;
  }
}

// ── bulk
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("bulk push and pop", "[mpsc][bulk]") {
  MpscRing<int, 8> q;
  std::array<int, 8> in{0, 1, 2, 3, 4, 5, 6, 7};

  CHECK(q.TryPushBulk(in.data(), 8) == 8);
  CHECK(q.TryPushBulk(in.data(), 1) == 0);  // full

  std::array<int, 8> out{};
  CHECK(q.TryPopBulk(out.data(), 8) == 8);
  CHECK(out == in);
}

TEST_CASE("partial bulk push when near full", "[mpsc][bulk]") {
  MpscRing<int, 4> q;
  CHECK(q.TryPush(0));
  CHECK(q.TryPush(1));

  std::array<int, 4> in{2, 3, 4, 5};
  CHECK(q.TryPushBulk(in.data(), 4) == 2);  // only 2 slots free

  std::array<int, 4> out{};
  CHECK(q.TryPopBulk(out.data(), 4) == 4);
  CHECK(out[0] == 0);
  CHECK(out[1] == 1);
  CHECK(out[2] == 2);
  CHECK(out[3] == 3);
}

// ── concurrent MPSC
// ───────────────────────────────────────────────────────────

TEST_CASE("many producers, single consumer: no loss, no dup",
          "[mpsc][concurrent]") {
  constexpr std::size_t k_cap = 1024;
  constexpr int k_producers = 4;
  constexpr int k_per_prod = 50'000;
  constexpr int k_total = k_producers * k_per_prod;

  MpscRing<std::uint64_t, k_cap> q;

  // Encode (producer_id << 32 | seq) so we can verify per-producer ordering.
  std::atomic<bool> done{false};
  std::barrier sync(k_producers + 1);

  std::vector<std::thread> producers;
  for (int p = 0; p < k_producers; ++p) {
    producers.emplace_back([&, p] {
      sync.arrive_and_wait();
      for (int i = 0; i < k_per_prod; ++i) {
        std::uint64_t val = (static_cast<std::uint64_t>(p) << 32) |
                            static_cast<std::uint32_t>(i);
        while (!q.TryPush(val)) { /* spin, queue full */
        }
      }
    });
  }

  std::vector<int> next_expected(k_producers, 0);
  long consumed = 0;
  bool ordering_ok = true;

  std::thread consumer([&] {
    sync.arrive_and_wait();
    while (consumed < k_total) {
      auto v = q.TryPop();
      if (!v.has_value()) {
        continue;
      }
      int pid = static_cast<int>(*v >> 32);
      int seq = static_cast<int>(*v & 0xFFFF'FFFF);
      if (seq != next_expected[pid]) {
        ordering_ok = false;
      }
      ++next_expected[pid];
      ++consumed;
    }
  });

  for (auto& t : producers) {
    t.join();
  }
  consumer.join();

  CHECK(consumed == k_total);
  CHECK(ordering_ok);
  for (int p = 0; p < k_producers; ++p) {
    CHECK(next_expected[p] == k_per_prod);
  }
}
