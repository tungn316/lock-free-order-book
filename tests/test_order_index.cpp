#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "lfob/order_index.hpp"
#include "lfob/types.hpp"

using namespace lfob;

namespace {

NodeRef Ref(NodeIdx idx, Generation gen = 0) { return NodeRef{.idx = idx, .gen = gen}; }

bool Same(const NodeRef* got, NodeRef want) {
  return got != nullptr && got->idx == want.idx && got->gen == want.gen;
}

}  // namespace

// ── basics ─────────────────────────────────────────────────────────────────

TEST_CASE("fresh index is empty and finds nothing", "[index][single]") {
  OrderIndex idx(16);
  CHECK(idx.Size() == 0);
  CHECK(idx.Find(1) == nullptr);
  CHECK(idx.Find(0) == nullptr);
}

TEST_CASE("Put then Find returns the stored ref", "[index][single]") {
  OrderIndex idx(16);
  idx.Put(42, Ref(7, 3));
  REQUIRE(idx.Size() == 1);
  CHECK(Same(idx.Find(42), Ref(7, 3)));
  CHECK(idx.Find(43) == nullptr);
}

TEST_CASE("OrderId 0 is a normal key (empty is encoded in the ref, not the id)",
          "[index][single]") {
  OrderIndex idx(16);
  idx.Put(0, Ref(5));
  CHECK(Same(idx.Find(0), Ref(5)));
  CHECK(idx.Size() == 1);
  idx.Erase(0);
  CHECK(idx.Find(0) == nullptr);
  CHECK(idx.Size() == 0);
}

TEST_CASE("Put on an existing id overwrites without growing", "[index][single]") {
  OrderIndex idx(16);
  idx.Put(42, Ref(7));
  idx.Put(42, Ref(9, 1));
  CHECK(idx.Size() == 1);
  CHECK(Same(idx.Find(42), Ref(9, 1)));
}

TEST_CASE("Erase removes the key and decrements size", "[index][single]") {
  OrderIndex idx(16);
  idx.Put(42, Ref(7));
  idx.Erase(42);
  CHECK(idx.Size() == 0);
  CHECK(idx.Find(42) == nullptr);
}

TEST_CASE("Erase of an absent key is a no-op", "[index][single]") {
  OrderIndex idx(16);
  idx.Put(42, Ref(7));
  idx.Erase(999);
  CHECK(idx.Size() == 1);
  CHECK(Same(idx.Find(42), Ref(7)));
}

// ── probe chains / backward-shift ───────────────────────────────────────────

TEST_CASE("colliding keys all remain findable, and erasing a middle one keeps "
          "the rest reachable",
          "[index][probe]") {
  // Small capacity -> small table (next pow2 >= 2*cap). Many keys guarantee
  // long probe chains, exercising the backward-shift path on Erase.
  constexpr std::size_t k_cap = 32;
  OrderIndex idx(k_cap);

  std::vector<OrderId> ids;
  for (OrderId id = 1; id <= k_cap; ++id) {
    ids.push_back(id);
    idx.Put(id, Ref(static_cast<NodeIdx>(id)));
  }
  REQUIRE(idx.Size() == k_cap);
  for (const OrderId id : ids) {
    CHECK(Same(idx.Find(id), Ref(static_cast<NodeIdx>(id))));
  }

  // Erase every third key; the survivors must all still resolve.
  for (std::size_t i = 0; i < ids.size(); i += 3) {
    idx.Erase(ids[i]);
  }
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i % 3 == 0) {
      CHECK(idx.Find(ids[i]) == nullptr);
    } else {
      CHECK(Same(idx.Find(ids[i]), Ref(static_cast<NodeIdx>(ids[i]))));
    }
  }
}

TEST_CASE("index fills exactly to capacity and every key is retrievable",
          "[index][probe]") {
  constexpr std::size_t k_cap = 100;
  OrderIndex idx(k_cap);
  for (OrderId id = 1; id <= k_cap; ++id) {
    idx.Put(id, Ref(static_cast<NodeIdx>(id), static_cast<Generation>(id * 2)));
  }
  REQUIRE(idx.Size() == k_cap);
  for (OrderId id = 1; id <= k_cap; ++id) {
    CHECK(Same(idx.Find(id),
               Ref(static_cast<NodeIdx>(id), static_cast<Generation>(id * 2))));
  }
}

// ── oracle stress ───────────────────────────────────────────────────────────

TEST_CASE("random Put/Erase churn matches a std::unordered_map oracle",
          "[index][stress]") {
  constexpr std::size_t k_cap = 512;
  OrderIndex idx(k_cap);
  std::unordered_map<OrderId, NodeRef> oracle;

  std::mt19937_64 rng(0xC0FFEE);
  std::uniform_int_distribution<OrderId> key(1, 4000);  // sparse-ish key space
  std::vector<OrderId> live;  // keys currently in the map, for random erase

  Generation gen = 0;
  for (int step = 0; step < 200'000; ++step) {
    const bool do_put = oracle.empty() ||
                        (oracle.size() < k_cap && (rng() & 1U) != 0U);
    if (do_put) {
      const OrderId id = key(rng);
      const NodeRef r = Ref(static_cast<NodeIdx>(rng() % k_cap), ++gen);
      const bool is_new = !oracle.contains(id);
      idx.Put(id, r);
      oracle[id] = r;
      if (is_new) {
        live.push_back(id);
      }
    } else {
      // Erase a random live key.
      std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
      const std::size_t slot = pick(rng);
      const OrderId id = live[slot];
      idx.Erase(id);
      oracle.erase(id);
      live[slot] = live.back();
      live.pop_back();
    }

    REQUIRE(idx.Size() == oracle.size());
  }

  // Final agreement over every key that was ever touched.
  for (const auto& [id, ref] : oracle) {
    REQUIRE(Same(idx.Find(id), ref));
  }
  for (OrderId id = 1; id <= 4000; ++id) {
    if (!oracle.contains(id)) {
      REQUIRE(idx.Find(id) == nullptr);
    }
  }
}
