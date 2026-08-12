#include <vector>
#include <algorithm>
#include <unordered_set>

#include <catch2/catch_test_macros.hpp>

#include "lfob/node_arena.hpp"
#include "lfob/types.hpp"

using namespace lfob;

// ── construction ──────────────────────────────────────────────────────────────

TEST_CASE("fresh arena reports full capacity and nothing in use",
          "[arena][single]") {
    NodeArena arena(16);
    CHECK(arena.Capacity() == 16);
    CHECK(arena.InUse() == 0);
    CHECK_FALSE(arena.Exhausted());
}

// ── acquire / release ─────────────────────────────────────────────────────────

TEST_CASE("Acquire hands out a valid ref and bumps InUse", "[arena][single]") {
    NodeArena arena(4);
    const NodeRef ref = arena.Acquire();
    CHECK(ref.idx != k_null_node);
    CHECK(ref.idx < arena.Capacity());
    CHECK(arena.InUse() == 1);
}

TEST_CASE("Acquire returns distinct indices until exhausted",
          "[arena][single]") {
    constexpr std::size_t k_cap = 8;
    NodeArena arena(k_cap);

    std::unordered_set<NodeIdx> seen;
    for (std::size_t i = 0; i < k_cap; ++i) {
        const NodeRef ref = arena.Acquire();
        REQUIRE(ref.idx != k_null_node);
        CHECK(seen.insert(ref.idx).second);  // no duplicates
    }
    CHECK(seen.size() == k_cap);
    CHECK(arena.InUse() == k_cap);
    CHECK(arena.Exhausted());
}

TEST_CASE("Acquire on an exhausted arena returns the null-tagged ref",
          "[arena][single]") {
    NodeArena arena(2);
    (void)arena.Acquire();
    (void)arena.Acquire();
    REQUIRE(arena.Exhausted());

    const NodeRef ref = arena.Acquire();
    CHECK(ref.idx == k_null_node);
    CHECK(ref.gen == 0);
    CHECK(arena.InUse() == 2);  // unchanged — arena never grows
}

TEST_CASE("Release returns a slot to the free list", "[arena][single]") {
    NodeArena arena(4);
    const NodeRef ref = arena.Acquire();
    CHECK(arena.InUse() == 1);

    arena.Release(ref.idx);
    CHECK(arena.InUse() == 0);
    CHECK_FALSE(arena.Exhausted());
}

TEST_CASE("released slot is recycled by the next Acquire", "[arena][single]") {
    NodeArena arena(1);
    const NodeRef first = arena.Acquire();
    REQUIRE(arena.Exhausted());

    arena.Release(first.idx);
    const NodeRef second = arena.Acquire();

    CHECK(second.idx == first.idx);  // LIFO reuse of the only slot
}

// ── generation / staleness ─────────────────────────────────────────────────────

TEST_CASE("Release bumps the slot generation", "[arena][generation]") {
    NodeArena arena(4);
    const NodeRef ref = arena.Acquire();
    const Generation before = arena.GenerationOf(ref.idx);

    arena.Release(ref.idx);
    CHECK(arena.GenerationOf(ref.idx) == before + 1);
}

TEST_CASE("recycled Acquire carries the bumped generation",
          "[arena][generation]") {
    NodeArena arena(1);
    const NodeRef first = arena.Acquire();
    arena.Release(first.idx);
    const NodeRef second = arena.Acquire();

    CHECK(second.idx == first.idx);
    CHECK(second.gen == first.gen + 1);
}

// ── Resolve ─────────────────────────────────────────────────────────────────────

TEST_CASE("Resolve returns a live pointer for a current ref",
          "[arena][resolve]") {
    NodeArena arena(4);
    const NodeRef ref = arena.Acquire();

    Order* ptr = arena.Resolve(ref);
    REQUIRE(ptr != nullptr);

    ptr->id = 7;
    ptr->price = 123;
    CHECK(arena[ref.idx].id == 7);
    CHECK(arena[ref.idx].price == 123);
}

TEST_CASE("Resolve rejects a stale ref after release", "[arena][resolve]") {
    NodeArena arena(4);
    const NodeRef ref = arena.Acquire();
    REQUIRE(arena.Resolve(ref) != nullptr);

    arena.Release(ref.idx);
    CHECK(arena.Resolve(ref) == nullptr);  // generation moved on
}

TEST_CASE("Resolve rejects an out-of-range index", "[arena][resolve]") {
    NodeArena arena(4);
    const NodeRef bad{.idx = 999, .gen = 0};
    CHECK(arena.Resolve(bad) == nullptr);
}

TEST_CASE("null-tagged ref from an exhausted arena resolves to nullptr",
          "[arena][resolve]") {
    NodeArena arena(1);
    (void)arena.Acquire();
    const NodeRef null_ref = arena.Acquire();
    REQUIRE(null_ref.idx == k_null_node);
    CHECK(arena.Resolve(null_ref) == nullptr);
}

// ── churn / stress ──────────────────────────────────────────────────────────────

TEST_CASE("acquire/release churn never leaks or double-hands a slot",
          "[arena][stress]") {
    constexpr std::size_t k_cap = 64;
    NodeArena arena(k_cap);

    std::vector<NodeRef> live;
    live.reserve(k_cap);

    // Drain fully.
    for (std::size_t i = 0; i < k_cap; ++i) {
        live.push_back(arena.Acquire());
    }
    REQUIRE(arena.Exhausted());

    // Repeated release-then-reacquire cycles.
    for (int round = 0; round < 1'000; ++round) {
        arena.Release(live.back().idx);
        live.pop_back();
        CHECK(arena.InUse() == k_cap - 1);

        const NodeRef ref = arena.Acquire();
        REQUIRE(ref.idx != k_null_node);
        live.push_back(ref);
        CHECK(arena.InUse() == k_cap);
    }

    // All handed-out indices remain unique.
    std::unordered_set<NodeIdx> seen;
    for (const auto& ref : live) {
        CHECK(seen.insert(ref.idx).second);
    }
    CHECK(seen.size() == k_cap);
}

TEST_CASE("stale refs stay invalid across a full recycle", "[arena][stress]") {
    NodeArena arena(4);

    const NodeRef old = arena.Acquire();
    arena.Release(old.idx);

    // Force the slot through several generations.
    for (int i = 0; i < 8; ++i) {
        const NodeRef ref = arena.Acquire();
        arena.Release(ref.idx);
    }
    CHECK(arena.Resolve(old) == nullptr);
}
