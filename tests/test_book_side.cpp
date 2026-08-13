#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "lfob/book_side.hpp"
#include "lfob/types.hpp"

using namespace lfob;

// ── helpers ──────────────────────────────────────────────────────────────────
namespace {

// A bid book spanning [min, max]. Convenience factory so each test reads
// clearly about which side it exercises.
inline BookSide MakeBid(Price min_price, Price max_price) {
  return BookSide(Side::BID, min_price, max_price);
}

inline BookSide MakeAsk(Price min_price, Price max_price) {
  return BookSide(Side::ASK, min_price, max_price);
}

}  // namespace

// ── construction / geometry
// ─────────────────────────────────────────────────

TEST_CASE("fresh book side is empty", "[bookside][single]") {
  BookSide bs = MakeBid(100, 200);
  CHECK(bs.Empty());
  CHECK(bs.Best() == nullptr);
}

TEST_CASE("InRange respects inclusive bounds", "[bookside][single]") {
  BookSide bs = MakeBid(100, 200);
  CHECK_FALSE(bs.InRange(99));
  CHECK(bs.InRange(100));
  CHECK(bs.InRange(150));
  CHECK(bs.InRange(200));
  CHECK_FALSE(bs.InRange(201));
}

TEST_CASE("IndexOf and PriceOf are inverses", "[bookside][single]") {
  BookSide bs = MakeBid(100, 200);
  for (Price p = 100; p <= 200; ++p) {
    const std::uint32_t idx = bs.IndexOf(p);
    CHECK(bs.PriceOf(idx) == p);
  }
  CHECK(bs.IndexOf(100) == 0);
  CHECK(bs.IndexOf(200) == 100);
}

TEST_CASE("single price range is valid", "[bookside][single]") {
  BookSide bs = MakeBid(100, 100);
  CHECK(bs.InRange(100));
  CHECK_FALSE(bs.InRange(101));
  bs.MarkOccupied(100);
  CHECK_FALSE(bs.Empty());
  CHECK(bs.BestPrice() == 100);
}

// ── best tracking: BID (higher price = better)
// ──────────────────────────────

TEST_CASE("bid best is the highest occupied price", "[bookside][bid]") {
  BookSide bs = MakeBid(100, 200);

  bs.MarkOccupied(150);
  CHECK(bs.BestPrice() == 150);

  bs.MarkOccupied(120);  // worse, must not move best
  CHECK(bs.BestPrice() == 150);

  bs.MarkOccupied(180);  // better, must move best
  CHECK(bs.BestPrice() == 180);
}

TEST_CASE("bid best advances downward when emptied", "[bookside][bid]") {
  BookSide bs = MakeBid(100, 200);
  bs.MarkOccupied(120);
  bs.MarkOccupied(150);
  bs.MarkOccupied(180);
  REQUIRE(bs.BestPrice() == 180);

  bs.OnLevelEmptied(bs.IndexOf(180));
  CHECK(bs.BestPrice() == 150);

  bs.OnLevelEmptied(bs.IndexOf(150));
  CHECK(bs.BestPrice() == 120);

  bs.OnLevelEmptied(bs.IndexOf(120));
  CHECK(bs.Empty());
  CHECK(bs.Best() == nullptr);
}

TEST_CASE("bid: emptying a non-best level does not disturb best",
          "[bookside][bid]") {
  BookSide bs = MakeBid(100, 200);
  bs.MarkOccupied(120);
  bs.MarkOccupied(180);
  REQUIRE(bs.BestPrice() == 180);

  bs.OnLevelEmptied(bs.IndexOf(120));
  CHECK(bs.BestPrice() == 180);
}

// ── best tracking: ASK (lower price = better)
// ───────────────────────────────

TEST_CASE("ask best is the lowest occupied price", "[bookside][ask]") {
  BookSide bs = MakeAsk(100, 200);

  bs.MarkOccupied(150);
  CHECK(bs.BestPrice() == 150);

  bs.MarkOccupied(180);  // worse, must not move best
  CHECK(bs.BestPrice() == 150);

  bs.MarkOccupied(120);  // better, must move best
  CHECK(bs.BestPrice() == 120);
}

TEST_CASE("ask best advances upward when emptied", "[bookside][ask]") {
  BookSide bs = MakeAsk(100, 200);
  bs.MarkOccupied(120);
  bs.MarkOccupied(150);
  bs.MarkOccupied(180);
  REQUIRE(bs.BestPrice() == 120);

  bs.OnLevelEmptied(bs.IndexOf(120));
  CHECK(bs.BestPrice() == 150);

  bs.OnLevelEmptied(bs.IndexOf(150));
  CHECK(bs.BestPrice() == 180);

  bs.OnLevelEmptied(bs.IndexOf(180));
  CHECK(bs.Empty());
}

// ── bitmap word-boundary scanning
// ───────────────────────────────────────────

TEST_CASE("bid AdvanceBest crosses 64-bit word boundaries",
          "[bookside][bid][bitmap]") {
  // Range large enough to span several bitmap words.
  BookSide bs = MakeBid(0, 300);

  // Occupy prices that straddle word boundaries (bits 63/64, 127/128).
  bs.MarkOccupied(63);
  bs.MarkOccupied(64);
  bs.MarkOccupied(128);
  bs.MarkOccupied(200);
  REQUIRE(bs.BestPrice() == 200);

  bs.OnLevelEmptied(bs.IndexOf(200));
  CHECK(bs.BestPrice() == 128);

  bs.OnLevelEmptied(bs.IndexOf(128));
  CHECK(bs.BestPrice() == 64);

  bs.OnLevelEmptied(bs.IndexOf(64));
  CHECK(bs.BestPrice() == 63);

  bs.OnLevelEmptied(bs.IndexOf(63));
  CHECK(bs.Empty());
}

TEST_CASE("ask AdvanceBest crosses 64-bit word boundaries",
          "[bookside][ask][bitmap]") {
  BookSide bs = MakeAsk(0, 300);

  bs.MarkOccupied(10);
  bs.MarkOccupied(63);
  bs.MarkOccupied(64);
  bs.MarkOccupied(129);
  REQUIRE(bs.BestPrice() == 10);

  bs.OnLevelEmptied(bs.IndexOf(10));
  CHECK(bs.BestPrice() == 63);

  bs.OnLevelEmptied(bs.IndexOf(63));
  CHECK(bs.BestPrice() == 64);

  bs.OnLevelEmptied(bs.IndexOf(64));
  CHECK(bs.BestPrice() == 129);

  bs.OnLevelEmptied(bs.IndexOf(129));
  CHECK(bs.Empty());
}

// ── Crosses semantics
// ──────────────────────────────────────────────────────

TEST_CASE("empty book never crosses", "[bookside][crosses]") {
  BookSide bid = MakeBid(100, 200);
  BookSide ask = MakeAsk(100, 200);
  CHECK_FALSE(bid.Crosses(150));
  CHECK_FALSE(ask.Crosses(150));
}

TEST_CASE("bid crosses when incoming price <= best bid",
          "[bookside][crosses][bid]") {
  BookSide bs = MakeBid(100, 200);
  bs.MarkOccupied(150);  // best bid = 150

  CHECK(bs.Crosses(150));  // equal crosses
  CHECK(bs.Crosses(140));  // below crosses
  CHECK_FALSE(bs.Crosses(160));
}

TEST_CASE("ask crosses when incoming price >= best ask",
          "[bookside][crosses][ask]") {
  BookSide bs = MakeAsk(100, 200);
  bs.MarkOccupied(150);  // best ask = 150

  CHECK(bs.Crosses(150));  // equal crosses
  CHECK(bs.Crosses(160));  // above crosses
  CHECK_FALSE(bs.Crosses(140));
}

// ── Best() pointer identity
// ────────────────────────────────────────────────

TEST_CASE("Best() points at the level for the best price",
          "[bookside][single]") {
  BookSide bs = MakeBid(100, 200);
  bs.MarkOccupied(170);

  PriceLevel* best = bs.Best();
  REQUIRE(best != nullptr);
  CHECK(best == &bs.LevelAt(170));
}

TEST_CASE("re-occupying after full drain resets best correctly",
          "[bookside][single]") {
  BookSide bs = MakeAsk(100, 200);
  bs.MarkOccupied(150);
  bs.OnLevelEmptied(bs.IndexOf(150));
  REQUIRE(bs.Empty());

  bs.MarkOccupied(130);
  CHECK_FALSE(bs.Empty());
  CHECK(bs.BestPrice() == 130);
}
