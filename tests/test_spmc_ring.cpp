#include <array>
#include <atomic>
#include <barrier>
#include <optional>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "lfob/spmc_ring.hpp"
#include "lfob/types.hpp"

using namespace lfob;

// ── registration
// ──────────────────────────────────────────────────────────────

TEST_CASE("register up to k_max_consumers then fail", "[spmc][register]") {
  SpmcRing<int, 8> ring;
  for (std::size_t i = 0; i < k_max_consumers; ++i) {
    auto id = ring.RegisterConsumer();
    REQUIRE(id.has_value());
    CHECK(*id == i);
  }
  CHECK_FALSE(ring.RegisterConsumer().has_value());
}

// ── single-threaded broadcast
// ─────────────────────────────────────────────────

TEST_CASE("empty ring reads nullopt", "[spmc][single]") {
  SpmcRing<int, 8> ring;
  auto id = ring.RegisterConsumer();
  REQUIRE(id.has_value());
  CHECK_FALSE(ring.TryRead(*id).has_value());
  CHECK(ring.Lag(*id) == 0);
}

TEST_CASE("all consumers see every message", "[spmc][single]") {
  SpmcRing<int, 16> ring;
  auto a = ring.RegisterConsumer();
  auto b = ring.RegisterConsumer();
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());

  for (int i = 0; i < 10; ++i) {
    ring.Publish(i);
  }

  for (int i = 0; i < 10; ++i) {
    auto va = ring.TryRead(*a);
    auto vb = ring.TryRead(*b);
    REQUIRE(va.has_value());
    REQUIRE(vb.has_value());
    CHECK(*va == i);
    CHECK(*vb == i);
  }
  CHECK_FALSE(ring.TryRead(*a).has_value());
  CHECK_FALSE(ring.TryRead(*b).has_value());
}

TEST_CASE("Reserve/Commit writes in place", "[spmc][single]") {
  SpmcRing<int, 8> ring;
  auto id = ring.RegisterConsumer();
  REQUIRE(id.has_value());

  ring.Reserve() = 1234;
  ring.Commit();

  auto v = ring.TryRead(*id);
  REQUIRE(v.has_value());
  CHECK(*v == 1234);
}

// ── overflow / drop semantics
// ─────────────────────────────────────────────────

TEST_CASE("slow consumer is dropped and observes missed count",
          "[spmc][drop]") {
  constexpr std::size_t k_cap = 8;
  SpmcRing<int, k_cap> ring;
  auto id = ring.RegisterConsumer();
  REQUIRE(id.has_value());

  // Publish 2x capacity without reading. Consumer must be force-advanced.
  for (int i = 0; i < static_cast<int>(k_cap) * 2; ++i) {
    ring.Publish(i);
  }

  CHECK(ring.Missed(*id) > 0);
  // Missed is reset on read.
  CHECK(ring.Missed(*id) == 0);

  // Remaining reads must be the newest window, still monotonically increasing.
  int last = -1;
  while (auto v = ring.TryRead(*id)) {
    CHECK(*v > last);
    last = *v;
  }
  CHECK(last == static_cast<int>(k_cap) * 2 - 1);
}

// ── bulk
// ────────────────────────────────────────────────────────────────────── NOTE:
// This test targets INTENDED behavior. It will FAIL until the bug in
// TryReadBulk (n is never incremented) is fixed.

TEST_CASE("bulk read returns count and advances cursor", "[spmc][bulk]") {
  SpmcRing<int, 16> ring;
  auto id = ring.RegisterConsumer();
  REQUIRE(id.has_value());

  for (int i = 0; i < 10; ++i) {
    ring.Publish(i);
  }

  std::array<int, 16> out{};
  std::size_t n = ring.TryReadBulk(*id, out.data(), out.size());
  CHECK(n == 10);
  for (int i = 0; i < 10; ++i) {
    CHECK(out[i] == i);
  }
  CHECK(ring.TryReadBulk(*id, out.data(), out.size()) == 0);
}

// ── concurrent broadcast
// ──────────────────────────────────────────────────────

TEST_CASE("single producer, many consumers: monotonic, no gaps when keeping up",
          "[spmc][concurrent]") {
  constexpr std::size_t k_cap = 4096;
  constexpr int k_consumers = 4;
  constexpr int k_msgs = 200'000;

  SpmcRing<int, k_cap> ring;

  std::vector<SpmcRing<int, k_cap>::ConsumerId> ids;
  for (int i = 0; i < k_consumers; ++i) {
    auto id = ring.RegisterConsumer();
    REQUIRE(id.has_value());
    ids.push_back(*id);
  }

  std::barrier sync(k_consumers + 1);
  std::vector<int> last_seen(k_consumers, -1);
  std::vector<bool> monotonic(k_consumers, true);
  std::vector<long> received(k_consumers, 0);

  std::vector<std::thread> consumers;
  for (int c = 0; c < k_consumers; ++c) {
    consumers.emplace_back([&, c] {
      sync.arrive_and_wait();
      int last = -1;
      long got = 0;
      // Read until we've either seen the final value or the producer
      // has clearly finished and we've drained.
      while (last < k_msgs - 1) {
        auto v = ring.TryRead(ids[c]);
        if (!v.has_value()) {
          continue;
        }
        if (*v <= last) {
          monotonic[c] = false;
        }
        last = *v;
        ++got;
      }
      last_seen[c] = last;
      received[c] = got;
    });
  }

  std::thread producer([&] {
    sync.arrive_and_wait();
    for (int i = 0; i < k_msgs; ++i) {
      ring.Publish(i);
    }
  });

  producer.join();
  for (auto& t : consumers) {
    t.join();
  }

  for (int c = 0; c < k_consumers; ++c) {
    CHECK(monotonic[c]);
    CHECK(last_seen[c] == k_msgs - 1);
  }
}
