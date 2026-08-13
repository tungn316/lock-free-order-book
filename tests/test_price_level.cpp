#include <cstddef>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "lfob/node_arena.hpp"
#include "lfob/order.hpp"
#include "lfob/price_level.hpp"
#include "lfob/types.hpp"

using namespace lfob;

// ── helpers ──────────────────────────────────────────────────────────────────
namespace {

// Acquire a slot from the arena and stamp it with a remaining quantity.
// Returns the raw index; the level only ever traffics in indices.
inline NodeIdx MakeOrder(NodeArena& arena, Quantity remaining) {
  const NodeRef ref = arena.Acquire();
  REQUIRE(ref.idx != k_null_node);
  arena[ref.idx].remaining = remaining;
  return ref.idx;
}

// Walk the level head→tail via the arena links and collect indices in order.
// This is the ground truth for time priority: intrusive list order, not
// aggregate counters.
inline std::vector<NodeIdx> WalkForward(const NodeArena& arena,
                                        const PriceLevel& level) {
  std::vector<NodeIdx> out;
  NodeIdx cur = level.Front();
  while (cur != k_null_node) {
    out.push_back(cur);
    cur = arena[cur].next;
  }
  return out;
}

}  // namespace

// ── construction
// ──────────────────────────────────────────────────────────────

TEST_CASE("default-constructed level is empty", "[price_level][single]") {
  const PriceLevel level;
  CHECK(level.Empty());
  CHECK(level.OrderCount() == 0);
  CHECK(level.TotalQuantity() == 0);
  CHECK(level.Front() == k_null_node);
}

// ── push_back / ordering
// ──────────────────────────────────────────────────────

TEST_CASE("single PushBack updates head, counters, and links",
          "[price_level][single]") {
  NodeArena arena{16};
  PriceLevel level;

  const NodeIdx a = MakeOrder(arena, 10);
  level.PushBack(arena, a);

  CHECK_FALSE(level.Empty());
  CHECK(level.OrderCount() == 1);
  CHECK(level.TotalQuantity() == 10);
  CHECK(level.Front() == a);
  CHECK(arena[a].prev == k_null_node);
  CHECK(arena[a].next == k_null_node);
}

TEST_CASE("PushBack preserves FIFO time priority", "[price_level][single]") {
  NodeArena arena{16};
  PriceLevel level;

  const NodeIdx a = MakeOrder(arena, 1);
  const NodeIdx b = MakeOrder(arena, 2);
  const NodeIdx c = MakeOrder(arena, 3);
  level.PushBack(arena, a);
  level.PushBack(arena, b);
  level.PushBack(arena, c);

  CHECK(level.OrderCount() == 3);
  CHECK(level.TotalQuantity() == 6);
  CHECK(WalkForward(arena, level) == std::vector<NodeIdx>{a, b, c});
}

TEST_CASE("aggregate quantity accumulates across pushes",
          "[price_level][single]") {
  NodeArena arena{64};
  PriceLevel level;

  Quantity expected = 0;
  for (Quantity q = 1; q <= 20; ++q) {
    level.PushBack(arena, MakeOrder(arena, q));
    expected += q;
  }
  CHECK(level.OrderCount() == 20);
  CHECK(level.TotalQuantity() == expected);
}

// ── remove
// ────────────────────────────────────────────────────────────────────

TEST_CASE("Remove from middle relinks neighbours", "[price_level][single]") {
  NodeArena arena{16};
  PriceLevel level;

  const NodeIdx a = MakeOrder(arena, 5);
  const NodeIdx b = MakeOrder(arena, 7);
  const NodeIdx c = MakeOrder(arena, 9);
  level.PushBack(arena, a);
  level.PushBack(arena, b);
  level.PushBack(arena, c);

  level.Remove(arena, b);

  CHECK(level.OrderCount() == 2);
  CHECK(level.TotalQuantity() == 14);
  CHECK(WalkForward(arena, level) == std::vector<NodeIdx>{a, c});
  CHECK(arena[a].next == c);
  CHECK(arena[c].prev == a);
  // Removed node is fully detached.
  CHECK(arena[b].prev == k_null_node);
  CHECK(arena[b].next == k_null_node);
}

TEST_CASE("Remove head updates Front", "[price_level][single]") {
  NodeArena arena{16};
  PriceLevel level;

  const NodeIdx a = MakeOrder(arena, 5);
  const NodeIdx b = MakeOrder(arena, 7);
  level.PushBack(arena, a);
  level.PushBack(arena, b);

  level.Remove(arena, a);

  CHECK(level.Front() == b);
  CHECK(arena[b].prev == k_null_node);
  CHECK(level.OrderCount() == 1);
  CHECK(level.TotalQuantity() == 7);
}

TEST_CASE("Remove tail then PushBack links correctly",
          "[price_level][single]") {
  NodeArena arena{16};
  PriceLevel level;

  const NodeIdx a = MakeOrder(arena, 5);
  const NodeIdx b = MakeOrder(arena, 7);
  level.PushBack(arena, a);
  level.PushBack(arena, b);

  level.Remove(arena, b);  // b was tail

  const NodeIdx c = MakeOrder(arena, 3);
  level.PushBack(arena, c);  // must attach after a, not stale b

  CHECK(WalkForward(arena, level) == std::vector<NodeIdx>{a, c});
  CHECK(arena[a].next == c);
  CHECK(arena[c].prev == a);
  CHECK(level.TotalQuantity() == 8);
}

TEST_CASE("Remove only element empties the level", "[price_level][single]") {
  NodeArena arena{16};
  PriceLevel level;

  const NodeIdx a = MakeOrder(arena, 5);
  level.PushBack(arena, a);
  level.Remove(arena, a);

  CHECK(level.Empty());
  CHECK(level.OrderCount() == 0);
  CHECK(level.TotalQuantity() == 0);
  CHECK(level.Front() == k_null_node);
}

// ── pop_front
// ─────────────────────────────────────────────────────────────────

TEST_CASE("PopFront drains in FIFO order", "[price_level][single]") {
  NodeArena arena{16};
  PriceLevel level;

  const NodeIdx a = MakeOrder(arena, 1);
  const NodeIdx b = MakeOrder(arena, 2);
  const NodeIdx c = MakeOrder(arena, 3);
  level.PushBack(arena, a);
  level.PushBack(arena, b);
  level.PushBack(arena, c);

  CHECK(level.Front() == a);
  level.PopFront(arena);
  CHECK(level.Front() == b);
  CHECK(level.TotalQuantity() == 5);

  level.PopFront(arena);
  CHECK(level.Front() == c);
  CHECK(level.TotalQuantity() == 3);

  level.PopFront(arena);
  CHECK(level.Empty());
  CHECK(level.Front() == k_null_node);
}

// ── churn: push/remove interleaving keeps invariants ─────────────────────────

TEST_CASE("interleaved push/remove keeps counters and links consistent",
          "[price_level][churn]") {
  NodeArena arena{256};
  PriceLevel level;

  std::vector<NodeIdx> live;
  Quantity expected_qty = 0;

  // Fill.
  for (Quantity q = 1; q <= 50; ++q) {
    const NodeIdx n = MakeOrder(arena, q);
    level.PushBack(arena, n);
    live.push_back(n);
    expected_qty += q;
  }

  // Remove every other one.
  for (std::size_t i = 0; i < live.size(); i += 2) {
    const NodeIdx n = live[i];
    expected_qty -= arena[n].remaining;
    level.Remove(arena, n);
  }

  // Surviving nodes are the odd indices, still in FIFO order.
  std::vector<NodeIdx> survivors;
  for (std::size_t i = 1; i < live.size(); i += 2) {
    survivors.push_back(live[i]);
  }

  CHECK(level.OrderCount() == survivors.size());
  CHECK(level.TotalQuantity() == expected_qty);
  CHECK(WalkForward(arena, level) == survivors);
}
