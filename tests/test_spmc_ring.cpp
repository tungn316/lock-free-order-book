#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <numeric>
#include <optional>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "lfob/spmc_ring.hpp"
#include "lfob/types.hpp"

using namespace lfob;

// ── single-threaded
// ───────────────────────────────────────────────────────────

TEST_CASE("empty ring reads nothing", "[spmc][single]") {
  SpmcRing<int, 4> ring;
  auto a = ring.Register();

  CHECK_FALSE(ring.TryRead(a).has_value());

  std::array<int, 4> out{};
  CHECK(ring.TryReadBulk(a, out.data(), out.size()) == 0);
  CHECK(ring.SizeApprox() == 0);
}

TEST_CASE("single consumer sees every item in FIFO order", "[spmc][single]") {
  SpmcRing<int, 8> ring;
  auto a = ring.Register();

  for (int i = 0; i < 8; ++i) {
    CHECK(ring.TryPush(i));
  }
  for (int i = 0; i < 8; ++i) {
    auto v = ring.TryRead(a);
    REQUIRE(v.has_value());
    CHECK(*v == i);
  }
  CHECK_FALSE(ring.TryRead(a).has_value());
}

TEST_CASE("broadcast is fan-out, not work-sharing", "[spmc][broadcast]") {
  SpmcRing<int, 8> ring;
  auto a = ring.Register();
  auto b = ring.Register();

  for (int i = 0; i < 3; ++i) {
    CHECK(ring.TryPush(100 + i));
  }

  auto next = [&](auto id) {
    auto v = ring.TryRead(id);
    REQUIRE(v.has_value());
    return *v;
  };

  // A drains the whole stream first.
  CHECK(next(a) == 100);
  CHECK(next(a) == 101);
  CHECK(next(a) == 102);
  CHECK_FALSE(ring.TryRead(a).has_value());

  // B still receives the full stream: A did not consume on B's behalf.
  CHECK(next(b) == 100);
  CHECK(next(b) == 101);
  CHECK(next(b) == 102);
  CHECK_FALSE(ring.TryRead(b).has_value());
}

TEST_CASE("push fails when the ring is full", "[spmc][single]") {
  SpmcRing<int, 4> ring;
  auto a = ring.Register();

  for (int i = 0; i < 4; ++i) {
    CHECK(ring.TryPush(i));
  }
  CHECK_FALSE(ring.TryPush(99));  // full
  CHECK(ring.SizeApprox() == 4);

  auto v = ring.TryRead(a);
  REQUIRE(v.has_value());
  CHECK(*v == 0);
  CHECK(ring.TryPush(99));  // room now
}

TEST_CASE("producer gates on the slowest consumer", "[spmc][broadcast]") {
  SpmcRing<int, 4> ring;
  auto a = ring.Register();
  auto b = ring.Register();

  for (int i = 0; i < 4; ++i) {
    CHECK(ring.TryPush(i));  // fills the ring
  }
  CHECK_FALSE(ring.TryPush(99));

  // Drain A completely; B has read nothing.
  std::array<int, 4> out{};
  CHECK(ring.TryReadBulk(a, out.data(), out.size()) == 4);

  // Still full: the slowest consumer (B) has not moved, so no slot is free.
  CHECK_FALSE(ring.TryPush(99));
  CHECK(ring.SizeApprox() == 4);

  // Advance B by one -> exactly one slot frees up.
  auto v = ring.TryRead(b);
  REQUIRE(v.has_value());
  CHECK(*v == 0);
  CHECK(ring.TryPush(4));
  CHECK_FALSE(ring.TryPush(5));  // full again

  // FIFO continues across the wrap for the consumer that ran ahead.
  auto va = ring.TryRead(a);
  REQUIRE(va.has_value());
  CHECK(*va == 4);
}

TEST_CASE("wraparound preserves FIFO order", "[spmc][single]") {
  SpmcRing<int, 4> ring;
  auto a = ring.Register();

  int next_push = 0;
  int next_pop = 0;
  for (int round = 0; round < 1000; ++round) {
    while (ring.TryPush(next_push)) {
      ++next_push;
    }
    auto v = ring.TryRead(a);
    REQUIRE(v.has_value());
    CHECK(*v == next_pop);
    ++next_pop;
  }
}

TEST_CASE("register hands out distinct ids", "[spmc][single]") {
  SpmcRing<int, 8, 4> ring;
  for (std::uint32_t i = 0; i < 4; ++i) {
    auto id = ring.Register();
    CHECK(id.idx == i);
  }
}

TEST_CASE("LagOf and SizeApprox track the slowest consumer",
          "[spmc][single]") {
  SpmcRing<int, 8> ring;
  auto a = ring.Register();
  auto b = ring.Register();

  for (int i = 0; i < 5; ++i) {
    CHECK(ring.TryPush(i));
  }

  std::array<int, 8> out{};
  CHECK(ring.TryReadBulk(a, out.data(), 2) == 2);  // A advances to 2, B stays at 0

  CHECK(ring.LagOf(a) == 3);
  CHECK(ring.LagOf(b) == 5);
  CHECK(ring.SizeApprox() == 5);  // == slowest lag
}

TEST_CASE("no consumers: producer never blocks", "[spmc][single]") {
  SpmcRing<int, 4> ring;  // nobody registered

  // With no gate, the producer overwrites freely and always succeeds.
  for (int i = 0; i < 3 * 4; ++i) {
    CHECK(ring.TryPush(i));
  }
  CHECK(ring.SizeApprox() == 0);  // no consumer => no backlog to report
}

// ── bulk
// ──────────────────────────────────────────────────────────────────────

TEST_CASE("bulk push and read", "[spmc][bulk]") {
  SpmcRing<int, 8> ring;
  auto a = ring.Register();
  std::array<int, 8> in{0, 1, 2, 3, 4, 5, 6, 7};

  CHECK(ring.TryPushBulk(in.data(), 8) == 8);
  CHECK(ring.TryPushBulk(in.data(), 1) == 0);  // full

  std::array<int, 8> out{};
  CHECK(ring.TryReadBulk(a, out.data(), 8) == 8);
  CHECK(out == in);
}

TEST_CASE("partial bulk push when near full", "[spmc][bulk]") {
  SpmcRing<int, 4> ring;
  auto a = ring.Register();
  CHECK(ring.TryPush(0));
  CHECK(ring.TryPush(1));

  std::array<int, 4> in{2, 3, 4, 5};
  CHECK(ring.TryPushBulk(in.data(), 4) == 2);  // only 2 slots free

  std::array<int, 4> out{};
  CHECK(ring.TryReadBulk(a, out.data(), 4) == 4);
  CHECK(out[0] == 0);
  CHECK(out[1] == 1);
  CHECK(out[2] == 2);
  CHECK(out[3] == 3);
}

TEST_CASE("bulk read caps at max and at available", "[spmc][bulk]") {
  SpmcRing<int, 8> ring;
  auto a = ring.Register();
  for (int i = 0; i < 5; ++i) {
    CHECK(ring.TryPush(i));
  }

  std::array<int, 8> out{};
  CHECK(ring.TryReadBulk(a, out.data(), 3) == 3);  // capped by max
  CHECK(out[0] == 0);
  CHECK(out[2] == 2);

  CHECK(ring.TryReadBulk(a, out.data(), 8) == 2);  // capped by what's left
  CHECK(out[0] == 3);
  CHECK(out[1] == 4);

  CHECK(ring.TryReadBulk(a, out.data(), 8) == 0);  // drained
}

TEST_CASE("bulk broadcast: every consumer drains the full batch",
          "[spmc][bulk][broadcast]") {
  SpmcRing<int, 8> ring;
  auto a = ring.Register();
  auto b = ring.Register();
  std::array<int, 8> in{0, 1, 2, 3, 4, 5, 6, 7};

  CHECK(ring.TryPushBulk(in.data(), 8) == 8);

  std::array<int, 8> oa{};
  std::array<int, 8> ob{};
  CHECK(ring.TryReadBulk(a, oa.data(), 8) == 8);
  CHECK(ring.TryReadBulk(b, ob.data(), 8) == 8);
  CHECK(oa == in);
  CHECK(ob == in);
}

// ── concurrent SPMC broadcast
// ────────────────────────────────────────────────

TEST_CASE("single producer, many consumers: full stream, in order, no loss",
          "[spmc][concurrent][broadcast]") {
  constexpr std::size_t k_cap = 1024;
  constexpr int k_consumers = 4;
  constexpr long k_total = 200'000;

  SpmcRing<std::uint64_t, k_cap, k_consumers> ring;

  std::array<SpmcRing<std::uint64_t, k_cap, k_consumers>::ConsumerId,
             k_consumers>
      ids{};
  for (int c = 0; c < k_consumers; ++c) {
    ids[c] = ring.Register();
  }

  std::barrier sync(k_consumers + 1);

  // Each consumer writes only its own slot -> distinct objects, no data race.
  std::array<long, k_consumers> counts{};
  std::array<bool, k_consumers> order_ok{};

  std::vector<std::thread> consumers;
  for (int c = 0; c < k_consumers; ++c) {
    consumers.emplace_back([&, c] {
      sync.arrive_and_wait();

      std::uint64_t expected = 0;
      long got = 0;
      bool ok = true;

      // Exercise both consumer paths: bulk Read on even ids (the path the
      // matching engine actually uses), single TryRead on odd ids.
      if (c % 2 == 0) {
        std::array<std::uint64_t, 64> buf{};
        while (got < k_total) {
          const std::size_t n = ring.TryReadBulk(ids[c], buf.data(), buf.size());
          for (std::size_t k = 0; k < n; ++k) {
            if (buf[k] != expected) {
              ok = false;
            }
            ++expected;
          }
          got += static_cast<long>(n);
        }
      } else {
        while (got < k_total) {
          auto v = ring.TryRead(ids[c]);
          if (!v.has_value()) {
            continue;
          }
          if (*v != expected) {
            ok = false;
          }
          ++expected;
          ++got;
        }
      }

      counts[c] = got;
      order_ok[c] = ok;
    });
  }

  std::thread producer([&] {
    sync.arrive_and_wait();
    for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(k_total); ++i) {
      while (!ring.TryPush(i)) { /* spin: slowest consumer gates */
      }
    }
  });

  producer.join();
  for (auto& t : consumers) {
    t.join();
  }

  // Lossless broadcast: every consumer received the entire stream, in order.
  for (int c = 0; c < k_consumers; ++c) {
    CHECK(counts[c] == k_total);
    CHECK(order_ok[c]);
  }
}
