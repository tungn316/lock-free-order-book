#ifndef ITCH_HPP_
#define ITCH_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include "types.hpp"

namespace lfob {

// NASDAQ TotalView-ITCH 5.0 over MoldUDP64 -- the wire layer that feeds the
// gateway. Just declarations here, everything is defined in src/itch.cpp.
//
// The feed comes in over UDP multicast, so it is lossy and batched. We peel it
// apart in three stages, and each one is testable on its own from a plain byte
// buffer (no socket, no engine needed):
//
//     NASDAQ multicast  (UDP: lossy, and duplicated across A/B feeds)
//            │
//            ▼   one raw datagram
//     ┌────────────────┐
//     │  MoldSession   │   datagram -> individual messages, put back in order,
//     └───────┬────────┘   with gap + duplicate detection
//             │   one message's bytes
//             ▼
//     ┌────────────────┐
//     │  DecodeItch    │   bytes -> ItchMessage  (flat POD, host byte order)
//     └───────┬────────┘
//             │   a typed message
//             ▼
//     ┌────────────────┐
//     │ ItchTranslator │   ItchMessage -> OrderCommand  (for our one symbol)
//     └───────┬────────┘
//             ▼
//        matching engine
//
// Everything on the wire is big-endian and unaligned, so we never overlay a
// struct on the raw bytes -- the decoder reads field by field with byte loads.
// No packing tricks, no aliasing, and no alignment fault even when a message
// starts on an odd offset inside the datagram.

// <---- MoldUDP64 framing ---->
//
// One datagram = a 20-byte header, then a run of length-prefixed messages:
//
//   ┌──────────────── header (20 bytes) ─────────────┐┌── msg 0 ──┐┌── msg 1 ──┐
//   │ session (10) │ sequence (8) │ message count (2) ││ len │ ... ││ len │ ... │
//   └──────────────┴──────┬───────┴───────────────────┘└─(2)─┴─────┘└─(2)─┴─────┘
//                         │
//                         └─ sequence of msg 0. msg 1 is sequence+1, and so on,
//                            so the whole datagram covers [sequence, sequence+count)
//
// Each message carries its own 2-byte length in front, so we can walk them
// without knowing any ITCH type -- handy, since we only decode a few of them.

// 10-byte session id, 8-byte sequence of the first message, 2-byte count
inline constexpr std::size_t k_mold_header_bytes{20};
inline constexpr std::size_t k_mold_session_bytes{10};

// Each message inside the datagram is prefixed with its own 2-byte length
inline constexpr std::size_t k_mold_length_prefix_bytes{2};

// A count of 0 is a heartbeat (sequence still advances the stream position by
// nothing); 0xFFFF ends the session for the day
inline constexpr std::uint16_t k_mold_end_of_session{0xFFFFU};

// Ethernet MTU minus IPv4 + UDP headers. Anything larger arrived fragmented
// and is not something this decoder should have been handed
inline constexpr std::size_t k_mold_max_datagram{1472};

// Largest ITCH 5.0 message body (Order Executed With Price, 36 bytes) with
// room to spare, so a staging buffer never has to grow
inline constexpr std::size_t k_itch_max_message_bytes{64};

struct MoldHeader {
  std::array<char, k_mold_session_bytes> session;
  SeqNum sequence;         // sequence of the first message in this datagram
  std::uint16_t message_count;
};

// <---- ITCH 5.0 messages ---->

// Only the message types that move a book or gate trading. The rest
// (directory, MWCB tiers, IPO quoting, RPII) decode to UNKNOWN and are
// dropped by the translator -- add them here when something needs them
enum class ItchType : char {
  UNKNOWN = '\0',
  SYSTEM_EVENT = 'S',
  STOCK_DIRECTORY = 'R',
  TRADING_ACTION = 'H',
  REG_SHO = 'Y',
  ADD_ORDER = 'A',
  ADD_ORDER_MPID = 'F',
  ORDER_EXECUTED = 'E',
  ORDER_EXECUTED_PRICE = 'C',
  ORDER_CANCEL = 'X',  // partial cancel: reduce shares, keep priority
  ORDER_DELETE = 'D',
  ORDER_REPLACE = 'U',
  TRADE = 'P',       // non-displayable execution, no book effect
  CROSS_TRADE = 'Q',
  BROKEN_TRADE = 'B',
};

// Body length of a message of this type, excluding the MoldUDP64 length
// prefix but including the type byte. Returns 0 for a type this build does
// not know, which is a skip, not an error: the length prefix already told us
// how far to advance
[[nodiscard]] std::size_t ItchBodyBytes(ItchType type) noexcept;

// Every decoded message in one flat trivially copyable struct.
//
// No union and no inheritance: it costs a few bytes per message that the
// decoder never populates, and buys a type that can be memcpy'd into a ring
// slot and read without knowing which arm is live. Fields are only meaningful
// for the types that carry them -- see the comments
struct ItchMessage {
  ItchType type;
  Side side;             // ADD_ORDER, ADD_ORDER_MPID ('B' -> BID, 'S' -> ASK)
  bool printable;        // ORDER_EXECUTED_PRICE, TRADE
  char event_code;       // SYSTEM_EVENT: 'O','S','Q','M','E','C'
  char trading_state;    // TRADING_ACTION: 'H' halted, 'T' trading, ...

  std::uint16_t stock_locate;      // symbol handle; the only filter that is free
  std::uint16_t tracking_number;

  // Nanoseconds since midnight Eastern, widened from the 48-bit wire field.
  // Not the same clock as NowNanos(); never subtract one from the other
  Timestamp timestamp;

  OrderId reference;           // order reference number
  OrderId original_reference;  // ORDER_REPLACE: the order being replaced
  OrderId match_number;        // execution / trade id

  Quantity shares;   // ADD_*, ORDER_EXECUTED*, ORDER_CANCEL (cancelled qty)
  Price price;       // raw ITCH price: 1/10000 of a dollar, NOT engine ticks

  std::array<char, 8> stock;  // space-padded, not NUL-terminated
};

static_assert(std::is_trivially_copyable_v<ItchMessage>);

enum class DecodeStatus : std::uint8_t {
  OK,
  TRUNCATED,       // fewer bytes than the type demands
  UNKNOWN_TYPE,    // well-framed, just not a type we decode
  LENGTH_MISMATCH  // framing says N bytes, the type says something else
};

// Decode one message body. `bytes` points at the type byte, `len` is the
// length the framing layer declared for it
[[nodiscard]] DecodeStatus DecodeItch(const std::byte* bytes,
                                      std::size_t len,
                                      ItchMessage& out) noexcept;

// <---- Feed session ---->

// Where a decoded feed lands. Implemented by the gateway's FeedHandler
class ItchSink {
 public:
  ItchSink() = default;
  virtual ~ItchSink() = default;

  ItchSink(const ItchSink&) = delete;
  ItchSink(ItchSink&&) = delete;
  ItchSink& operator=(const ItchSink&) = delete;
  ItchSink& operator=(ItchSink&&) = delete;

  virtual void OnMessage(const ItchMessage& msg) noexcept = 0;

  // Sequence discontinuity: [expected, received) never arrived. The book this
  // feed was driving is now wrong and cannot be repaired from the live stream
  // -- the recovery is a retransmission request or a fresh snapshot, and
  // until one lands the sink should stop trusting its state
  virtual void OnGap(SeqNum expected, SeqNum received) noexcept = 0;
};

// One MoldUDP64 stream, driven from a single receive thread.
//
// We keep m_expected = the next sequence number we want to see. Every datagram
// tells us the sequence it starts at (call it `first`), so working out whether
// it is in order, a duplicate, or a gap is just arithmetic -- no guessing:
//
//      first < m_expected        first == m_expected        first > m_expected
//   ┌────────────────────┐    ┌────────────────────┐    ┌────────────────────┐
//   │ already seen it     │    │ bang on -- deliver  │    │ GAP: the messages   │
//   │ (the other A/B      │    │ every message and   │    │ [m_expected, first) │
//   │  feed) -> skip it   │    │ bump m_expected by  │    │ never showed up     │
//   │  as a duplicate     │    │ the message count   │    │ -> raise OnGap       │
//   └────────────────────┘    └────────────────────┘    └────────────────────┘
//
// The A/B thing: the exchange sends two identical copies of the feed down
// separate lines. If A drops a packet, B's copy plugs the hole. So seeing a
// sequence below m_expected is completely normal, not an error -- we have just
// already processed it off the other feed, so we drop it.
class MoldSession {
 public:
  explicit MoldSession(ItchSink& sink) noexcept;
  ~MoldSession() = default;

  MoldSession(const MoldSession&) = delete;
  MoldSession(MoldSession&&) = delete;
  MoldSession& operator=(const MoldSession&) = delete;
  MoldSession& operator=(MoldSession&&) = delete;

  // Frame one datagram and push every message through the sink. Returns the
  // number of messages delivered; a malformed datagram is counted and dropped
  // whole rather than half-parsed
  std::size_t OnDatagram(const std::byte* datagram, std::size_t bytes) noexcept;

  [[nodiscard]] SeqNum Expected() const noexcept;
  [[nodiscard]] bool InSession() const noexcept;

  // Re-anchor after a recovery replay or a mid-day start. The next datagram is
  // accepted at `sequence` without raising a gap
  void Reset(SeqNum sequence) noexcept;

  // Monitoring. Never read from the receive loop
  [[nodiscard]] std::uint64_t Gaps() const noexcept;
  [[nodiscard]] std::uint64_t Duplicates() const noexcept;
  [[nodiscard]] std::uint64_t Malformed() const noexcept;

 private:
  [[nodiscard]] static bool ParseHeader(const std::byte* datagram,
                                        std::size_t bytes,
                                        MoldHeader& out) noexcept;

  // Compare the header sequence against m_expected and report a gap or a
  // duplicate. Returns how many messages of this datagram to skip
  [[nodiscard]] std::size_t Reconcile(const MoldHeader& header) noexcept;

  ItchSink& m_sink;
  std::array<char, k_mold_session_bytes> m_session{};
  SeqNum m_expected{0};
  bool m_anchored{false};  // false until the first datagram sets the session

  std::uint64_t m_gaps{0};
  std::uint64_t m_duplicates{0};
  std::uint64_t m_malformed{0};
};

// <---- Translation into engine commands ---->

// The catch with ITCH: it is an *outcome* feed, not an order feed. It tells us
// what the exchange already did -- there is no "an aggressive order just
// arrived" message, executions are the only proof one ever existed. So the
// translator has to decide what we are using the feed for:
//
//   BOOK_REBUILD    keep a book identical to NASDAQ's by replaying the maker
//                   side only:  A/F rest,  D cancels,  U replaces,  E/C/X reduce.
//                   Nothing ever crosses, so our own matching never runs
//   SYNTHETIC_FLOW  ignore the maker stream, treat the feed as pure market data
//                   and drive the engine with our own orders priced off it.
//                   This is the mode that actually exercises matching
//
// For the minimal decoded slice the mapping is just:
//
//        ITCH message              ItchTranslator            OrderCommand
//   ┌──────────────────┐        ┌────────────────┐        ┌───────────────┐
//   │ ADD_ORDER  'A'   │───────►│ symbol filter, │───────►│ NEW  (rests)  │
//   │ ORDER_DELETE 'D' │───────►│ tick + band    │───────►│ CANCEL (by id)│
//   │ SYSTEM_EVENT 'S' │───────►│ open/halt state│──X      (no command)   │
//   └──────────────────┘        └────────────────┘
//                                      │  everything else -> FILTERED / rejected
//                                      ▼
//   the ITCH order reference number becomes the engine order id, so a later
//   DELETE lines up with the ADD that created the order
enum class ReplayMode : std::uint8_t { BOOK_REBUILD, SYNTHETIC_FLOW };

class ItchTranslator {
 public:
  struct Config {
    ReplayMode mode;
    std::uint16_t stock_locate;  // the one symbol this book trades
    Price itch_per_tick;         // 1e-4 dollars per engine tick; 100 == 1 cent
    Price min_price;             // engine ticks, matches the book's band
    Price max_price;
    ClientId feed_client;  // stamped on every synthesized command so feed
                           // traffic is distinguishable from client traffic
  };

  explicit ItchTranslator(const Config& config) noexcept;
  ~ItchTranslator() = default;

  ItchTranslator(const ItchTranslator&) = delete;
  ItchTranslator(ItchTranslator&&) = delete;
  ItchTranslator& operator=(const ItchTranslator&) = delete;
  ItchTranslator& operator=(ItchTranslator&&) = delete;

  enum class Result : std::uint8_t {
    COMMAND,       // `out` is filled and ready for MatchingEngine::Submit
    FILTERED,      // different symbol, or a type with no book effect
    HALTED,        // symbol is not in a trading state right now
    OUT_OF_BAND,   // price outside [min_price, max_price] after scaling
    NOT_ON_TICK,   // price is not a whole number of engine ticks
    UNSUPPORTED,   // no faithful mapping exists -- see below
  };

  // ORDER_CANCEL ('X') is the honest gap: it reduces a resting order's shares
  // while keeping its time priority, and OrderCommand::Type has no REDUCE.
  // REPLACE would reset priority, which silently corrupts the queue position
  // of everything behind it, so 'X' returns UNSUPPORTED rather than lying.
  // Closing this means adding a priority-preserving reduce to the command set
  // and to OrderBook::Apply
  Result Translate(const ItchMessage& msg, OrderCommand& out) noexcept;

  // Feed-wide state the translator has to track to answer HALTED, and to know
  // whether the session is even open
  void OnSystemEvent(const ItchMessage& msg) noexcept;
  [[nodiscard]] bool Tradable() const noexcept;

  // Monitoring
  [[nodiscard]] std::uint64_t Unsupported() const noexcept;
  [[nodiscard]] std::uint64_t Rejected() const noexcept;

 private:
  // ITCH prices are unsigned 1e-4 dollars. Fails when the value is not a whole
  // multiple of itch_per_tick, rather than truncating an order onto the wrong
  // price level
  [[nodiscard]] bool ToTicks(Price itch_price, Price& ticks) const noexcept;

  Result TranslateAdd(const ItchMessage& msg, OrderCommand& out) noexcept;
  Result TranslateDelete(const ItchMessage& msg, OrderCommand& out) noexcept;
  Result TranslateReplace(const ItchMessage& msg, OrderCommand& out) noexcept;

  Config m_config;
  bool m_open{false};       // between the 'Q' and 'M' system events
  bool m_halted{false};     // per-symbol trading action
  std::uint64_t m_unsupported{0};
  std::uint64_t m_rejected{0};
};

}  // namespace lfob

#endif  // ITCH_HPP_
