#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "lfob/itch.hpp"
#include "lfob/types.hpp"

using namespace lfob;

namespace {

// Append a big-endian integer of `width` bytes
void PutBE(std::vector<std::byte>& v, std::uint64_t value, std::size_t width) {
  for (std::size_t i{0}; i < width; ++i) {
    const unsigned shift{static_cast<unsigned>((width - 1 - i) * 8)};
    v.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void PutChar(std::vector<std::byte>& v, char c) {
  v.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
}

// Build a well-formed System Event ('S', 12 bytes)
std::vector<std::byte> SystemEvent(char event_code) {
  std::vector<std::byte> m;
  PutChar(m, 'S');
  PutBE(m, 7, 2);          // stock locate
  PutBE(m, 3, 2);          // tracking number
  PutBE(m, 123456789, 6);  // timestamp
  PutChar(m, event_code);
  return m;
}

// Build a well-formed Add Order ('A', 36 bytes)
std::vector<std::byte> AddOrder(std::uint64_t ref, char side, std::uint32_t shares,
                                std::uint32_t price) {
  std::vector<std::byte> m;
  PutChar(m, 'A');
  PutBE(m, 42, 2);          // stock locate
  PutBE(m, 1, 2);           // tracking number
  PutBE(m, 999, 6);         // timestamp
  PutBE(m, ref, 8);         // order reference
  PutChar(m, side);         // buy/sell
  PutBE(m, shares, 4);      // shares
  for (int i = 0; i < 8; ++i) {
    PutChar(m, "AAPL    "[i]);  // stock, space-padded
  }
  PutBE(m, price, 4);       // price
  return m;
}

// Build a well-formed Order Delete ('D', 19 bytes)
std::vector<std::byte> OrderDelete(std::uint64_t ref) {
  std::vector<std::byte> m;
  PutChar(m, 'D');
  PutBE(m, 42, 2);
  PutBE(m, 1, 2);
  PutBE(m, 999, 6);
  PutBE(m, ref, 8);
  return m;
}

// Build an Add Order with MPID ('F', 40 bytes): 'A' plus a trailing 4-byte MPID
std::vector<std::byte> AddOrderMpid(std::uint64_t ref, char side,
                                    std::uint32_t shares, std::uint32_t price) {
  std::vector<std::byte> m;
  PutChar(m, 'F');
  PutBE(m, 42, 2);
  PutBE(m, 1, 2);
  PutBE(m, 999, 6);
  PutBE(m, ref, 8);
  PutChar(m, side);
  PutBE(m, shares, 4);
  for (int i = 0; i < 8; ++i) {
    PutChar(m, "MSFT    "[i]);
  }
  PutBE(m, price, 4);
  for (int i = 0; i < 4; ++i) {
    PutChar(m, "MPID"[i]);  // attribution, ignored by the decoder
  }
  return m;
}

// Build a Trading Action ('H', 25 bytes)
std::vector<std::byte> TradingAction(char state) {
  std::vector<std::byte> m;
  PutChar(m, 'H');
  PutBE(m, 42, 2);
  PutBE(m, 1, 2);
  PutBE(m, 999, 6);
  for (int i = 0; i < 8; ++i) {
    PutChar(m, "AAPL    "[i]);
  }
  PutChar(m, state);   // trading state @ offset 19
  PutBE(m, 0, 5);      // reserved (1) + reason (4)
  return m;
}

}  // namespace

TEST_CASE("ItchBodyBytes reports lengths for the decoded slice", "[itch]") {
  CHECK(ItchBodyBytes(ItchType::SYSTEM_EVENT) == 12);
  CHECK(ItchBodyBytes(ItchType::TRADING_ACTION) == 25);
  CHECK(ItchBodyBytes(ItchType::ADD_ORDER) == 36);
  CHECK(ItchBodyBytes(ItchType::ADD_ORDER_MPID) == 40);
  CHECK(ItchBodyBytes(ItchType::ORDER_DELETE) == 19);
  // Still not decoded -> 0 (a skip, not an error)
  CHECK(ItchBodyBytes(ItchType::ORDER_REPLACE) == 0);
  CHECK(ItchBodyBytes(ItchType::TRADE) == 0);
  CHECK(ItchBodyBytes(ItchType::UNKNOWN) == 0);
}

TEST_CASE("DecodeItch reads a system event", "[itch]") {
  const auto m{SystemEvent('O')};
  ItchMessage out{};
  REQUIRE(DecodeItch(m.data(), m.size(), out) == DecodeStatus::OK);
  CHECK(out.type == ItchType::SYSTEM_EVENT);
  CHECK(out.stock_locate == 7);
  CHECK(out.tracking_number == 3);
  CHECK(out.timestamp == 123456789);
  CHECK(out.event_code == 'O');
}

TEST_CASE("DecodeItch reads an add order with all fields", "[itch]") {
  const auto m{AddOrder(0xDEADBEEF, 'B', 500, 101500)};
  ItchMessage out{};
  REQUIRE(DecodeItch(m.data(), m.size(), out) == DecodeStatus::OK);
  CHECK(out.type == ItchType::ADD_ORDER);
  CHECK(out.stock_locate == 42);
  CHECK(out.reference == 0xDEADBEEF);
  CHECK(out.side == Side::BID);
  CHECK(out.shares == 500);
  CHECK(out.price == 101500);
  CHECK(out.stock == std::array<char, 8>{'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '});
}

TEST_CASE("DecodeItch maps buy/sell to side", "[itch]") {
  ItchMessage out{};
  const auto bid{AddOrder(1, 'B', 1, 1)};
  REQUIRE(DecodeItch(bid.data(), bid.size(), out) == DecodeStatus::OK);
  CHECK(out.side == Side::BID);

  const auto ask{AddOrder(1, 'S', 1, 1)};
  REQUIRE(DecodeItch(ask.data(), ask.size(), out) == DecodeStatus::OK);
  CHECK(out.side == Side::ASK);
}

TEST_CASE("DecodeItch reads an order delete", "[itch]") {
  const auto m{OrderDelete(0xABCD)};
  ItchMessage out{};
  REQUIRE(DecodeItch(m.data(), m.size(), out) == DecodeStatus::OK);
  CHECK(out.type == ItchType::ORDER_DELETE);
  CHECK(out.reference == 0xABCD);
}

TEST_CASE("DecodeItch reads an MPID add like a plain add", "[itch]") {
  const auto m{AddOrderMpid(0xBEEF, 'S', 250, 202000)};
  ItchMessage out{};
  REQUIRE(DecodeItch(m.data(), m.size(), out) == DecodeStatus::OK);
  CHECK(out.type == ItchType::ADD_ORDER_MPID);
  CHECK(out.reference == 0xBEEF);
  CHECK(out.side == Side::ASK);
  CHECK(out.shares == 250);
  CHECK(out.price == 202000);  // trailing MPID does not disturb the fields
}

TEST_CASE("DecodeItch reads a trading action", "[itch]") {
  const auto halt{TradingAction('H')};
  ItchMessage out{};
  REQUIRE(DecodeItch(halt.data(), halt.size(), out) == DecodeStatus::OK);
  CHECK(out.type == ItchType::TRADING_ACTION);
  CHECK(out.trading_state == 'H');

  const auto trade{TradingAction('T')};
  REQUIRE(DecodeItch(trade.data(), trade.size(), out) == DecodeStatus::OK);
  CHECK(out.trading_state == 'T');
}

TEST_CASE("DecodeItch rejects an unknown type", "[itch]") {
  std::vector<std::byte> m;
  PutChar(m, 'Z');  // not a type we decode
  PutBE(m, 0, 11);
  ItchMessage out{};
  CHECK(DecodeItch(m.data(), m.size(), out) == DecodeStatus::UNKNOWN_TYPE);
}

TEST_CASE("DecodeItch reports truncation", "[itch]") {
  ItchMessage out{};
  CHECK(DecodeItch(nullptr, 0, out) == DecodeStatus::TRUNCATED);

  const auto full{AddOrder(1, 'B', 1, 1)};
  // Same type byte, but fewer bytes than an add order needs
  CHECK(DecodeItch(full.data(), 20, out) == DecodeStatus::TRUNCATED);
}

TEST_CASE("DecodeItch reports a length mismatch", "[itch]") {
  const auto m{OrderDelete(1)};  // type 'D', truly 19 bytes
  ItchMessage out{};
  // Framing claims more bytes than a delete occupies
  CHECK(DecodeItch(m.data(), 25, out) == DecodeStatus::LENGTH_MISMATCH);
}

// ── MoldUDP64 framing ───────────────────────────────────────────────────────

namespace {

constexpr char kSess[]{"SESSION001"};  // 10 chars + NUL

// Records what MoldSession pushes through the sink
struct RecordingSink : ItchSink {
  std::vector<ItchMessage> messages;
  std::vector<std::pair<SeqNum, SeqNum>> gaps;
  void OnMessage(const ItchMessage& m) noexcept override { messages.push_back(m); }
  void OnGap(SeqNum expected, SeqNum received) noexcept override {
    gaps.emplace_back(expected, received);
  }
};

// Build a MoldUDP64 datagram: 10-byte session, 8-byte sequence, 2-byte count,
// then each message as a 2-byte length prefix + body
std::vector<std::byte> Datagram(SeqNum seq, std::uint16_t count,
                                const std::vector<std::vector<std::byte>>& msgs) {
  std::vector<std::byte> d;
  for (std::size_t i{0}; i < 10; ++i) {
    PutChar(d, kSess[i]);
  }
  PutBE(d, seq, 8);
  PutBE(d, count, 2);
  for (const auto& m : msgs) {
    PutBE(d, m.size(), 2);
    for (const auto b : m) {
      d.push_back(b);
    }
  }
  return d;
}

}  // namespace

TEST_CASE("MoldSession anchors on the first datagram and delivers", "[itch][mold]") {
  RecordingSink sink;
  MoldSession mold{sink};

  const auto d{Datagram(100, 2, {AddOrder(1, 'B', 10, 100), OrderDelete(1)})};
  const std::size_t n{mold.OnDatagram(d.data(), d.size())};

  CHECK(n == 2);
  CHECK(sink.messages.size() == 2);
  CHECK(mold.InSession());
  CHECK(mold.Expected() == 102);  // first(100) + count(2)
  CHECK(mold.Gaps() == 0);
}

TEST_CASE("MoldSession delivers in-order datagrams", "[itch][mold]") {
  RecordingSink sink;
  MoldSession mold{sink};

  const auto d1{Datagram(1, 1, {AddOrder(1, 'B', 10, 100)})};
  const auto d2{Datagram(2, 1, {OrderDelete(1)})};
  mold.OnDatagram(d1.data(), d1.size());
  const std::size_t n{mold.OnDatagram(d2.data(), d2.size())};

  CHECK(n == 1);
  CHECK(sink.messages.size() == 2);
  CHECK(mold.Expected() == 3);
  CHECK(mold.Gaps() == 0);
  CHECK(mold.Duplicates() == 0);
}

TEST_CASE("MoldSession drops a fully duplicate datagram", "[itch][mold]") {
  RecordingSink sink;
  MoldSession mold{sink};

  const auto d{Datagram(1, 2, {AddOrder(1, 'B', 10, 100), OrderDelete(1)})};
  mold.OnDatagram(d.data(), d.size());
  const std::size_t n{mold.OnDatagram(d.data(), d.size())};  // replayed A/B feed

  CHECK(n == 0);
  CHECK(sink.messages.size() == 2);  // no re-delivery
  CHECK(mold.Duplicates() == 1);
  CHECK(mold.Expected() == 3);
}

TEST_CASE("MoldSession delivers only the new tail of a partial duplicate",
          "[itch][mold]") {
  RecordingSink sink;
  MoldSession mold{sink};

  const auto d1{Datagram(1, 2, {AddOrder(1, 'B', 10, 100), OrderDelete(1)})};
  mold.OnDatagram(d1.data(), d1.size());  // expected -> 3

  // Overlaps: seq 2 already seen, seq 3 is new
  const auto d2{Datagram(2, 2, {OrderDelete(2), AddOrder(3, 'S', 5, 200)})};
  const std::size_t n{mold.OnDatagram(d2.data(), d2.size())};

  CHECK(n == 1);                       // only the seq-3 message
  CHECK(sink.messages.size() == 3);
  CHECK(mold.Duplicates() == 1);
  CHECK(mold.Expected() == 4);
}

TEST_CASE("MoldSession reports a gap and re-anchors", "[itch][mold]") {
  RecordingSink sink;
  MoldSession mold{sink};

  const auto d1{Datagram(1, 1, {AddOrder(1, 'B', 10, 100)})};
  mold.OnDatagram(d1.data(), d1.size());  // expected -> 2

  // Jump to seq 5: messages 2,3,4 never arrived
  const auto d2{Datagram(5, 1, {OrderDelete(5)})};
  const std::size_t n{mold.OnDatagram(d2.data(), d2.size())};

  CHECK(n == 1);
  REQUIRE(sink.gaps.size() == 1);
  CHECK(sink.gaps[0] == std::pair<SeqNum, SeqNum>{2, 5});
  CHECK(mold.Gaps() == 1);
  CHECK(mold.Expected() == 6);  // re-anchored past the gap
}

TEST_CASE("MoldSession treats a heartbeat as gap-detection only", "[itch][mold]") {
  RecordingSink sink;
  MoldSession mold{sink};

  const auto d1{Datagram(1, 1, {AddOrder(1, 'B', 10, 100)})};
  mold.OnDatagram(d1.data(), d1.size());  // expected -> 2

  const auto hb{Datagram(2, 0, {})};  // heartbeat at the expected sequence
  const std::size_t n{mold.OnDatagram(hb.data(), hb.size())};

  CHECK(n == 0);
  CHECK(mold.Gaps() == 0);
  CHECK(mold.Duplicates() == 0);  // an on-time heartbeat is not a duplicate

  const auto hb_ahead{Datagram(9, 0, {})};  // heartbeat past expected -> gap
  mold.OnDatagram(hb_ahead.data(), hb_ahead.size());
  CHECK(mold.Gaps() == 1);
  CHECK(mold.Expected() == 9);
}

TEST_CASE("MoldSession counts a short header as malformed", "[itch][mold]") {
  RecordingSink sink;
  MoldSession mold{sink};

  std::array<std::byte, 10> tiny{};  // < 20-byte header
  const std::size_t n{mold.OnDatagram(tiny.data(), tiny.size())};

  CHECK(n == 0);
  CHECK(mold.Malformed() == 1);
}

TEST_CASE("MoldSession drops a datagram whose message overruns it whole",
          "[itch][mold]") {
  RecordingSink sink;
  MoldSession mold{sink};

  // Header claims 2 messages but only one body is present
  auto d{Datagram(1, 1, {AddOrder(1, 'B', 10, 100)})};
  // Rewrite the count field (offset 18-19) to claim 2 messages
  d[18] = std::byte{0};
  d[19] = std::byte{2};
  const std::size_t n{mold.OnDatagram(d.data(), d.size())};

  CHECK(n == 0);                 // nothing delivered
  CHECK(sink.messages.empty());  // dropped whole, not half-parsed
  CHECK(mold.Malformed() == 1);
}

TEST_CASE("MoldSession end-of-session closes the stream", "[itch][mold]") {
  RecordingSink sink;
  MoldSession mold{sink};

  const auto d1{Datagram(1, 1, {AddOrder(1, 'B', 10, 100)})};
  mold.OnDatagram(d1.data(), d1.size());
  CHECK(mold.InSession());

  const auto eos{Datagram(2, 0xFFFF, {})};
  const std::size_t n{mold.OnDatagram(eos.data(), eos.size())};

  CHECK(n == 0);
  CHECK_FALSE(mold.InSession());
}

TEST_CASE("MoldSession Reset re-anchors without a gap", "[itch][mold]") {
  RecordingSink sink;
  MoldSession mold{sink};

  const auto d1{Datagram(1, 1, {AddOrder(1, 'B', 10, 100)})};
  mold.OnDatagram(d1.data(), d1.size());  // expected -> 2

  mold.Reset(1000);  // recovery replay jumps the stream forward
  CHECK(mold.Expected() == 1000);

  const auto d2{Datagram(1000, 1, {OrderDelete(1)})};
  const std::size_t n{mold.OnDatagram(d2.data(), d2.size())};
  CHECK(n == 1);
  CHECK(mold.Gaps() == 0);  // Reset absorbed the jump, no gap raised
}

// ── ItchTranslator ──────────────────────────────────────────────────────────

namespace {

constexpr std::uint16_t kLocate{42};

ItchTranslator::Config TranslatorConfig() {
  return ItchTranslator::Config{
      .mode = ReplayMode::BOOK_REBUILD,
      .stock_locate = kLocate,
      .itch_per_tick = 100,  // 1 cent per engine tick
      .min_price = 1,
      .max_price = 100000,
      .feed_client = 999,
  };
}

// A decoded ITCH message built directly (bypassing the wire), for translator
// tests that need types the minimal decoder does not produce
ItchMessage Msg(ItchType type) {
  ItchMessage m{};
  m.type = type;
  m.stock_locate = kLocate;
  return m;
}

}  // namespace

TEST_CASE("Translate maps an add order to a NEW command", "[itch][xlate]") {
  ItchTranslator xlate{TranslatorConfig()};
  ItchMessage msg{Msg(ItchType::ADD_ORDER)};
  msg.side = Side::BID;
  msg.reference = 555;
  msg.shares = 300;
  msg.price = 101500;  // 1e-4 dollars -> 1015 ticks

  OrderCommand out{};
  REQUIRE(xlate.Translate(msg, out) == ItchTranslator::Result::COMMAND);
  CHECK(out.type == OrderCommand::Type::NEW);
  CHECK(out.side == Side::BID);
  CHECK(out.tif == TimeInForce::DAY);
  CHECK(out.client == 999);
  CHECK(out.id == 555);
  CHECK(out.price == 1015);
  CHECK(out.quantity == 300);
}

TEST_CASE("Translate maps an order delete to a CANCEL command", "[itch][xlate]") {
  ItchTranslator xlate{TranslatorConfig()};
  ItchMessage msg{Msg(ItchType::ORDER_DELETE)};
  msg.reference = 777;

  OrderCommand out{};
  REQUIRE(xlate.Translate(msg, out) == ItchTranslator::Result::COMMAND);
  CHECK(out.type == OrderCommand::Type::CANCEL);
  CHECK(out.id == 777);
  CHECK(out.client == 999);
}

TEST_CASE("Translate filters a different symbol", "[itch][xlate]") {
  ItchTranslator xlate{TranslatorConfig()};
  ItchMessage msg{Msg(ItchType::ADD_ORDER)};
  msg.stock_locate = kLocate + 1;  // not our book
  msg.price = 101500;

  OrderCommand out{};
  CHECK(xlate.Translate(msg, out) == ItchTranslator::Result::FILTERED);
}

TEST_CASE("Translate rejects a price off the tick grid", "[itch][xlate]") {
  ItchTranslator xlate{TranslatorConfig()};
  ItchMessage msg{Msg(ItchType::ADD_ORDER)};
  msg.price = 101550;  // 101550 % 100 != 0

  OrderCommand out{};
  CHECK(xlate.Translate(msg, out) == ItchTranslator::Result::NOT_ON_TICK);
  CHECK(xlate.Rejected() == 1);
}

TEST_CASE("Translate rejects a price out of band", "[itch][xlate]") {
  ItchTranslator xlate{TranslatorConfig()};
  ItchMessage msg{Msg(ItchType::ADD_ORDER)};
  msg.price = 100000000;  // 1,000,000 ticks > max_price (100000)

  OrderCommand out{};
  CHECK(xlate.Translate(msg, out) == ItchTranslator::Result::OUT_OF_BAND);
  CHECK(xlate.Rejected() == 1);
}

TEST_CASE("Translate tracks market open/close via system events", "[itch][xlate]") {
  ItchTranslator xlate{TranslatorConfig()};
  CHECK_FALSE(xlate.Tradable());  // not open until 'Q'

  OrderCommand out{};
  ItchMessage open{Msg(ItchType::SYSTEM_EVENT)};
  open.event_code = 'Q';
  CHECK(xlate.Translate(open, out) == ItchTranslator::Result::FILTERED);
  CHECK(xlate.Tradable());

  ItchMessage close{Msg(ItchType::SYSTEM_EVENT)};
  close.event_code = 'M';
  xlate.OnSystemEvent(close);
  CHECK_FALSE(xlate.Tradable());
}

TEST_CASE("Translate halts adds during a trading halt", "[itch][xlate]") {
  ItchTranslator xlate{TranslatorConfig()};

  ItchMessage halt{Msg(ItchType::TRADING_ACTION)};
  halt.trading_state = 'H';
  xlate.OnSystemEvent(halt);

  ItchMessage add{Msg(ItchType::ADD_ORDER)};
  add.price = 101500;
  OrderCommand out{};
  CHECK(xlate.Translate(add, out) == ItchTranslator::Result::HALTED);
  CHECK(xlate.Rejected() == 1);

  // A delete still goes through -- an order can always be pulled
  ItchMessage del{Msg(ItchType::ORDER_DELETE)};
  del.reference = 5;
  CHECK(xlate.Translate(del, out) == ItchTranslator::Result::COMMAND);
}

TEST_CASE("Translate reports replace as unsupported for now", "[itch][xlate]") {
  ItchTranslator xlate{TranslatorConfig()};
  ItchMessage msg{Msg(ItchType::ORDER_REPLACE)};

  OrderCommand out{};
  CHECK(xlate.Translate(msg, out) == ItchTranslator::Result::UNSUPPORTED);
  CHECK(xlate.Unsupported() == 1);
}

TEST_CASE("a decoded trading halt gates a decoded add end to end",
          "[itch][xlate]") {
  ItchTranslator xlate{TranslatorConfig()};
  OrderCommand out{};

  // Decode a real 'H' halt off the wire and run it through the translator
  const auto halt{TradingAction('H')};
  ItchMessage hmsg{};
  REQUIRE(DecodeItch(halt.data(), halt.size(), hmsg) == DecodeStatus::OK);
  CHECK(xlate.Translate(hmsg, out) == ItchTranslator::Result::FILTERED);

  // A subsequent add (also decoded) is now blocked by the halt
  const auto add{AddOrder(1, 'B', 100, 101500)};
  ItchMessage amsg{};
  REQUIRE(DecodeItch(add.data(), add.size(), amsg) == DecodeStatus::OK);
  CHECK(xlate.Translate(amsg, out) == ItchTranslator::Result::HALTED);

  // Resume trading, and the same add now produces a command
  const auto resume{TradingAction('T')};
  ItchMessage rmsg{};
  REQUIRE(DecodeItch(resume.data(), resume.size(), rmsg) == DecodeStatus::OK);
  xlate.Translate(rmsg, out);
  CHECK(xlate.Translate(amsg, out) == ItchTranslator::Result::COMMAND);
}
