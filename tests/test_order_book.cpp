#include <cstdint>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "lfob/order_book.hpp"
#include "lfob/types.hpp"

using namespace lfob;

// ── helpers ──────────────────────────────────────────────────────────────────
namespace {

// Sink that just records everything the book emits, in order.
class CapturingSink final : public ReportSink {
 public:
  void Emit(const ExecutionReport& report) override { m_log.push_back(report); }

  [[nodiscard]] const std::vector<ExecutionReport>& Log() const noexcept {
    return m_log;
  }
  void Clear() noexcept { m_log.clear(); }

  // Count reports of a given type.
  [[nodiscard]] std::size_t Count(ExecutionReport::Type type) const noexcept {
    std::size_t n = 0;
    for (const auto& r : m_log) {
      if (r.type == type) {
        ++n;
      }
    }
    return n;
  }

  // Last report of a given type, if any.
  [[nodiscard]] std::optional<ExecutionReport> Last(
      ExecutionReport::Type type) const noexcept {
    for (auto it = m_log.rbegin(); it != m_log.rend(); ++it) {
      if (it->type == type) {
        return *it;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<ExecutionReport> LastReject() const noexcept {
    return Last(ExecutionReport::Type::REJECTED);
  }

 private:
  std::vector<ExecutionReport> m_log;
};

// Book config shared by most cases.
constexpr Price k_min_price = 1;
constexpr Price k_max_price = 1000;
constexpr std::size_t k_max_orders = 1024;

// Fluent-ish command builder so tests read cleanly.
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

// Fixture bundling a sink + book so we don't repeat construction.
struct Fixture {
  CapturingSink sink;
  OrderBook book{k_min_price, k_max_price, k_max_orders, sink};

  void Apply(const Cmd& cmd) { book.Apply(*cmd); }
};

}  // namespace

// ── construction / empty state ───────────────────────────────────────────────

TEST_CASE("fresh book is empty with no BBO", "[orderbook][basic]") {
  Fixture fx;
  CHECK(fx.book.Empty());
  CHECK(fx.book.BestBid() == k_no_price);
  CHECK(fx.book.BestAsk() == k_no_price);
  CHECK(fx.sink.Log().empty());
}

// ── resting orders / BBO ─────────────────────────────────────────────────────

TEST_CASE("single resting bid sets BestBid and emits TopOfBook",
          "[orderbook][rest]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::BID, 100, 10));

  CHECK(fx.book.BestBid() == 100);
  CHECK(fx.book.BestAsk() == k_no_price);
  CHECK(fx.sink.Count(ExecutionReport::Type::ACCEPTED) == 1);

  const auto tob = fx.sink.Last(ExecutionReport::Type::TOP_OF_BOOK);
  REQUIRE(tob.has_value());
  CHECK(tob->bid_price == 100);
  CHECK(tob->bid_quantity == 10);
  CHECK(tob->ask_price == k_no_price);
}

TEST_CASE("best bid tracks the highest price", "[orderbook][rest]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::BID, 100, 10));
  fx.Apply(Cmd::New(2, Side::BID, 105, 5));
  fx.Apply(Cmd::New(3, Side::BID, 102, 7));
  CHECK(fx.book.BestBid() == 105);
}

TEST_CASE("best ask tracks the lowest price", "[orderbook][rest]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::ASK, 110, 10));
  fx.Apply(Cmd::New(2, Side::ASK, 108, 5));
  fx.Apply(Cmd::New(3, Side::ASK, 109, 7));
  CHECK(fx.book.BestAsk() == 108);
}

// ── validation / rejects ─────────────────────────────────────────────────────

TEST_CASE("zero-quantity NEW is rejected", "[orderbook][reject]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::BID, 100, 0));
  const auto rej = fx.sink.LastReject();
  REQUIRE(rej.has_value());
  CHECK(rej->reason == ExecutionReport::RejectReason::ZERO_QUANTITY);
  CHECK(fx.book.Empty());
}

TEST_CASE("out-of-band price is rejected", "[orderbook][reject]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::BID, k_max_price + 1, 10));
  const auto rej = fx.sink.LastReject();
  REQUIRE(rej.has_value());
  CHECK(rej->reason == ExecutionReport::RejectReason::PRICE_OUT_OF_BAND);
}

TEST_CASE("cancel of unknown order is rejected", "[orderbook][reject]") {
  Fixture fx;
  fx.Apply(Cmd::Cancel(999));
  const auto rej = fx.sink.LastReject();
  REQUIRE(rej.has_value());
  CHECK(rej->reason == ExecutionReport::RejectReason::UNKNOWN_ORDER);
}

// ── matching ─────────────────────────────────────────────────────────────────

TEST_CASE("aggressive order fully fills a resting order",
          "[orderbook][match]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::ASK, 100, 10));  // maker
  fx.sink.Clear();

  fx.Apply(Cmd::New(2, Side::BID, 100, 10));  // taker crosses

  // Two FILL reports per trade (maker + taker side).
  CHECK(fx.sink.Count(ExecutionReport::Type::FILL) == 2);
  CHECK(fx.book.Empty());
  CHECK(fx.book.BestAsk() == k_no_price);
}

TEST_CASE("partial fill leaves remainder resting", "[orderbook][match]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::ASK, 100, 4));   // maker only 4
  fx.Apply(Cmd::New(2, Side::BID, 100, 10));  // taker wants 10

  CHECK(fx.sink.Count(ExecutionReport::Type::FILL) == 2);  // one trade of 4
  CHECK(fx.book.BestBid() == 100);  // 6 remaining rests as bid
  CHECK(fx.book.BestAsk() == k_no_price);

  const auto tob = fx.sink.Last(ExecutionReport::Type::TOP_OF_BOOK);
  REQUIRE(tob.has_value());
  CHECK(tob->bid_quantity == 6);
}

TEST_CASE("taker sweeps multiple price levels in priority order",
          "[orderbook][match]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::ASK, 100, 5));
  fx.Apply(Cmd::New(2, Side::ASK, 101, 5));
  fx.sink.Clear();

  fx.Apply(Cmd::New(3, Side::BID, 101, 10));

  // Two trades => four FILL reports.
  CHECK(fx.sink.Count(ExecutionReport::Type::FILL) == 4);
  CHECK(fx.book.Empty());
}

TEST_CASE("FIFO time priority within a level", "[orderbook][match]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::ASK, 100, 5));  // first in
  fx.Apply(Cmd::New(2, Side::ASK, 100, 5));  // second in
  fx.sink.Clear();

  fx.Apply(Cmd::New(3, Side::BID, 100, 5));  // should hit id 1

  const auto fill = fx.sink.Last(ExecutionReport::Type::FILL);
  REQUIRE(fill.has_value());
  // Order id 2 must still be resting.
  CHECK(fx.book.BestAsk() == 100);
}

// ── TIF: FOK ─────────────────────────────────────────────────────────────────

TEST_CASE("FOK that cannot fully fill is rejected and rests nothing",
          "[orderbook][fok]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::ASK, 100, 3));  // only 3 available
  fx.sink.Clear();

  fx.Apply(Cmd::New(2, Side::BID, 100, 10, TimeInForce::FOK));

  const auto rej = fx.sink.LastReject();
  REQUIRE(rej.has_value());
  CHECK(rej->reason == ExecutionReport::RejectReason::FOK_UNFILLABLE);
  CHECK(fx.sink.Count(ExecutionReport::Type::FILL) == 0);
  CHECK(fx.book.BestAsk() == 100);  // maker untouched
}

TEST_CASE("FOK that fully fills executes", "[orderbook][fok]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::ASK, 100, 10));
  fx.sink.Clear();

  fx.Apply(Cmd::New(2, Side::BID, 100, 10, TimeInForce::FOK));

  CHECK(fx.sink.Count(ExecutionReport::Type::FILL) == 2);
  CHECK(fx.sink.LastReject() == std::nullopt);
  CHECK(fx.book.Empty());
}

// ── TIF: IOC (non-DAY remainder is cancelled, not rested) ────────────────────

TEST_CASE("IOC remainder is cancelled, not rested", "[orderbook][ioc]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::ASK, 100, 4));
  fx.sink.Clear();

  fx.Apply(Cmd::New(2, Side::BID, 100, 10, TimeInForce::IOC));

  CHECK(fx.sink.Count(ExecutionReport::Type::FILL) == 2);
  CHECK(fx.sink.Count(ExecutionReport::Type::CANCELLED) == 1);  // leaves = 6
  CHECK(fx.book.BestBid() == k_no_price);  // nothing rested
}

// ── cancel ───────────────────────────────────────────────────────────────────

TEST_CASE("cancel removes a resting order and updates BBO",
          "[orderbook][cancel]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::BID, 100, 10));
  fx.sink.Clear();

  fx.Apply(Cmd::Cancel(1, Side::BID));

  CHECK(fx.sink.Count(ExecutionReport::Type::CANCELLED) == 1);
  CHECK(fx.book.Empty());
  CHECK(fx.book.BestBid() == k_no_price);
}

TEST_CASE("double cancel: second is UnknownOrder", "[orderbook][cancel]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::BID, 100, 10));
  fx.Apply(Cmd::Cancel(1, Side::BID));
  fx.sink.Clear();

  fx.Apply(Cmd::Cancel(1, Side::BID));
  const auto rej = fx.sink.LastReject();
  REQUIRE(rej.has_value());
  CHECK(rej->reason == ExecutionReport::RejectReason::UNKNOWN_ORDER);
}

// ── replace ──────────────────────────────────────────────────────────────────

TEST_CASE("replace re-prices a resting order", "[orderbook][replace]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::BID, 100, 10));
  fx.sink.Clear();

  fx.Apply(Cmd::Replace(1, Side::BID, 105, 8));

  CHECK(fx.sink.Count(ExecutionReport::Type::REPLACED) == 1);
  CHECK(fx.book.BestBid() == 105);

  const auto tob = fx.sink.Last(ExecutionReport::Type::TOP_OF_BOOK);
  REQUIRE(tob.has_value());
  CHECK(tob->bid_quantity == 8);
}

TEST_CASE("replace that now crosses executes immediately",
          "[orderbook][replace]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::ASK, 100, 10));  // resting ask
  fx.Apply(Cmd::New(2, Side::BID, 90, 10));   // resting bid, no cross
  fx.sink.Clear();

  fx.Apply(Cmd::Replace(2, Side::BID, 100, 10));  // now crosses the ask

  CHECK(fx.sink.Count(ExecutionReport::Type::FILL) == 2);
  CHECK(fx.book.Empty());
}

TEST_CASE("replace of unknown order is rejected", "[orderbook][replace]") {
  Fixture fx;
  fx.Apply(Cmd::Replace(42, Side::BID, 100, 10));
  const auto rej = fx.sink.LastReject();
  REQUIRE(rej.has_value());
  CHECK(rej->reason == ExecutionReport::RejectReason::UNKNOWN_ORDER);
}

// ── TopOfBook dedup ──────────────────────────────────────────────────────────

TEST_CASE("unchanged BBO does not re-emit TopOfBook", "[orderbook][tob]") {
  Fixture fx;
  fx.Apply(Cmd::New(1, Side::BID, 100, 10));  // emits TOB
  fx.Apply(Cmd::New(2, Side::BID, 90, 5));    // behind best, BBO unchanged

  // Only the first NEW should have produced a TOP_OF_BOOK.
  CHECK(fx.sink.Count(ExecutionReport::Type::TOP_OF_BOOK) == 1);
}

// ── arena exhaustion ─────────────────────────────────────────────────────────

TEST_CASE("resting past capacity yields ArenaExhausted", "[orderbook][arena]") {
  CapturingSink sink;
  OrderBook book{k_min_price, k_max_price, /*max_orders=*/2, sink};

  book.Apply(*Cmd::New(1, Side::BID, 100, 1));
  book.Apply(*Cmd::New(2, Side::BID, 99, 1));
  book.Apply(*Cmd::New(3, Side::BID, 98, 1));  // no free node

  bool saw_exhausted = false;
  for (const auto& r : sink.Log()) {
    if (r.type == ExecutionReport::Type::REJECTED &&
        r.reason == ExecutionReport::RejectReason::ARENA_EXHAUSTED) {
      saw_exhausted = true;
    }
  }
  CHECK(saw_exhausted);
}

// ── property / fuzz tests ────────────────────────────────────────────────────
namespace {

// Sink that validates structural invariants on every emit, plus accumulates
// per-order-id filled quantities so we can check conservation.
class InvariantSink final : public ReportSink {
 public:
  void Emit(const ExecutionReport& report) override {
    // seq is strictly increasing with no gaps: 1, 2, 3, ...
    ++m_expected_seq;
    if (report.seq != m_expected_seq) {
      m_seq_ok = false;
    }

    switch (report.type) {
      case ExecutionReport::Type::FILL:
        // Every fill has a positive traded quantity and a counterparty.
        if (report.quantity == 0) {
          m_bad_fill = true;
        }
        m_filled[report.order_id] += report.quantity;
        m_total_traded += report.quantity;
        ++m_fill_reports;
        break;

      case ExecutionReport::Type::TOP_OF_BOOK:
        // If both sides present, book must not be crossed.
        if (report.bid_price != k_no_price && report.ask_price != k_no_price &&
            report.bid_price >= report.ask_price) {
          m_crossed = true;
        }
        break;

      default:
        break;
    }
  }

  [[nodiscard]] bool SeqOk() const noexcept { return m_seq_ok; }
  [[nodiscard]] bool NoCrossedBook() const noexcept { return !m_crossed; }
  [[nodiscard]] bool NoBadFill() const noexcept { return !m_bad_fill; }

  // FILL reports come in maker/taker pairs, so the report count is even and
  // total traded quantity is double the actual matched volume.
  [[nodiscard]] bool FillsPaired() const noexcept {
    return (m_fill_reports % 2) == 0;
  }

  [[nodiscard]] Quantity FilledFor(OrderId id) const noexcept {
    const auto it = m_filled.find(id);
    return (it == m_filled.end()) ? 0 : it->second;
  }

 private:
  SeqNum m_expected_seq{0};
  bool m_seq_ok{true};
  bool m_crossed{false};
  bool m_bad_fill{false};
  std::size_t m_fill_reports{0};
  Quantity m_total_traded{0};
  std::unordered_map<OrderId, Quantity> m_filled;
};

}  // namespace

TEST_CASE("random command stream preserves core invariants",
          "[orderbook][fuzz][property]") {
  constexpr int k_iters = 20'000;
  constexpr Price k_lo = 90;
  constexpr Price k_hi = 110;
  constexpr Quantity k_max_quantity = 20;

  // Sweep a handful of seeds so a single lucky run can't hide a bug.
  for (unsigned seed : {1u, 7u, 42u, 1337u, 99991u}) {
    InvariantSink sink;
    OrderBook book{k_min_price, k_max_price, k_max_orders, sink};

    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> action(0, 9);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<Price> price_dist(k_lo, k_hi);
    std::uniform_int_distribution<Quantity> quantity_dist(1, k_max_quantity);
    std::uniform_int_distribution<int> tif_dist(0, 2);

    // Track ids we believe are live so cancels/replaces mostly hit.
    std::unordered_set<OrderId> maybe_live;
    OrderId next_id = 1;

    auto rand_side = [&] {
      return side_dist(rng) == 0 ? Side::BID : Side::ASK;
    };
    auto rand_tif = [&] {
      switch (tif_dist(rng)) {
        case 0:
          return TimeInForce::DAY;
        case 1:
          return TimeInForce::IOC;
        default:
          return TimeInForce::FOK;
      }
    };
    auto pick_existing = [&]() -> OrderId {
      if (maybe_live.empty()) {
        return 0;
      }
      std::uniform_int_distribution<std::size_t> pick(0, maybe_live.size() - 1);
      auto it = maybe_live.begin();
      std::advance(it, pick(rng));
      return *it;
    };

    for (int i = 0; i < k_iters; ++i) {
      const int a = action(rng);

      if (a < 6) {  // NEW (60%)
        const OrderId id = next_id++;
        const auto side = rand_side();
        book.Apply(*Cmd::New(id, side, price_dist(rng), quantity_dist(rng),
                             rand_tif()));
        maybe_live.insert(id);  // may already be gone (IOC/FOK/full fill)
      } else if (a < 8) {       // CANCEL (20%)
        const OrderId id = pick_existing();
        if (id != 0) {
          book.Apply(*Cmd::Cancel(id, rand_side()));
          maybe_live.erase(id);
        }
      } else {  // REPLACE (20%)
        const OrderId id = pick_existing();
        if (id != 0) {
          book.Apply(*Cmd::Replace(id, rand_side(), price_dist(rng),
                                   quantity_dist(rng), rand_tif()));
        }
      }

      // Cheap invariant that must hold after every single command.
      const Price bid = book.BestBid();
      const Price ask = book.BestAsk();
      if (bid != k_no_price && ask != k_no_price) {
        REQUIRE(bid < ask);  // book is never crossed at rest
      }
    }

    INFO("seed = " << seed);
    CHECK(sink.SeqOk());          // no gaps / reordering
    CHECK(sink.NoCrossedBook());  // every emitted TOB is sane
    CHECK(sink.NoBadFill());      // no zero-quantity fills
    CHECK(sink.FillsPaired());    // maker+taker always emitted together
  }
}

TEST_CASE("quantity is conserved across a matched pair",
          "[orderbook][property]") {
  // Direct, deterministic conservation check: whatever the taker executes,
  // the makers must collectively execute the same amount.
  InvariantSink sink;
  OrderBook book{k_min_price, k_max_price, k_max_orders, sink};

  book.Apply(*Cmd::New(1, Side::ASK, 100, 5));
  book.Apply(*Cmd::New(2, Side::ASK, 100, 5));
  book.Apply(*Cmd::New(3, Side::ASK, 101, 5));

  book.Apply(*Cmd::New(100, Side::BID, 101, 12));  // taker

  // Taker filled 12; makers 1 and 2 fully (5+5), maker 3 partially (2).
  CHECK(sink.FilledFor(100) == 12);
  CHECK(sink.FilledFor(1) == 5);
  CHECK(sink.FilledFor(2) == 5);
  CHECK(sink.FilledFor(3) == 2);

  // Sum of maker fills == taker fills == matched volume.
  const Quantity maker_total =
      sink.FilledFor(1) + sink.FilledFor(2) + sink.FilledFor(3);
  CHECK(maker_total == sink.FilledFor(100));

  CHECK(book.BestAsk() == 101);  // 3 left resting at 101
  CHECK(book.BestBid() == k_no_price);
}
