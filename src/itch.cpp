#include <lfob/itch.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lfob {

namespace {

// Everything on the ITCH wire is big-endian and unaligned, so every field is
// assembled from individual byte loads rather than overlaying a struct. These
// helpers read a big-endian integer of the given width starting at `p`.

std::uint8_t Byte(const std::byte* p) noexcept {
  return std::to_integer<std::uint8_t>(*p);
}

std::uint16_t ReadU16(const std::byte* p) noexcept {
  // Compute in `unsigned` so the shift operand never promotes to signed int
  const unsigned hi{Byte(p)};
  const unsigned lo{Byte(p + 1)};
  return static_cast<std::uint16_t>((hi << 8U) | lo);
}

std::uint32_t ReadU32(const std::byte* p) noexcept {
  return (static_cast<std::uint32_t>(Byte(p)) << 24U) |
         (static_cast<std::uint32_t>(Byte(p + 1)) << 16U) |
         (static_cast<std::uint32_t>(Byte(p + 2)) << 8U) |
         static_cast<std::uint32_t>(Byte(p + 3));
}

// 48-bit timestamp field, widened into a 64-bit host value
std::uint64_t ReadU48(const std::byte* p) noexcept {
  std::uint64_t v{0};
  for (std::size_t i{0}; i < 6; ++i) {
    v = (v << 8U) | static_cast<std::uint64_t>(Byte(p + i));
  }
  return v;
}

std::uint64_t ReadU64(const std::byte* p) noexcept {
  std::uint64_t v{0};
  for (std::size_t i{0}; i < 8; ++i) {
    v = (v << 8U) | static_cast<std::uint64_t>(Byte(p + i));
  }
  return v;
}

char ReadChar(const std::byte* p) noexcept {
  return static_cast<char>(Byte(p));
}

}  // namespace

// The wire layouts of the messages we actually decode. Every field is
// big-endian; `off` is the byte offset from the start of the message (the type
// byte is at 0). These offsets are exactly what DecodeItch reads below.
//
//   SYSTEM_EVENT 'S' (12 bytes)      ADD_ORDER 'A' (36 bytes) / 'F' (40, +MPID)
//   ┌─────┬──────┬───────────────┐   ┌─────┬──────┬───────────────────┐
//   │ off │ size │ field         │   │ off │ size │ field             │
//   ├─────┼──────┼───────────────┤   ├─────┼──────┼───────────────────┤
//   │  0  │   1  │ type 'S'      │   │  0  │   1  │ type 'A' or 'F'   │
//   │  1  │   2  │ stock locate  │   │  1  │   2  │ stock locate      │
//   │  3  │   2  │ tracking num  │   │  3  │   2  │ tracking num      │
//   │  5  │   6  │ timestamp     │   │  5  │   6  │ timestamp         │
//   │ 11  │   1  │ event code    │   │ 11  │   8  │ order reference   │
//   └─────┴──────┴───────────────┘   │ 19  │   1  │ buy/sell 'B'/'S'  │
//                                     │ 20  │   4  │ shares            │
//   ORDER_DELETE 'D' (19 bytes)       │ 24  │   8  │ stock (padded)    │
//   ┌─────┬──────┬───────────────┐   │ 32  │   4  │ price (1e-4 $)    │
//   │  0  │   1  │ type 'D'      │   │ 36  │   4  │ MPID  ('F' only,  │
//   │  1  │   2  │ stock locate  │   │     │      │        ignored)   │
//   │  3  │   2  │ tracking num  │   └─────┴──────┴───────────────────┘
//   │  5  │   6  │ timestamp     │
//   │ 11  │   8  │ order reference│   TRADING_ACTION 'H' (25 bytes)
//   └─────┴──────┴───────────────┘   ┌─────┬──────┬───────────────────┐
//                                     │  0  │   1  │ type 'H'          │
//   'F' shares the first 36 bytes     │  1  │   2  │ stock locate      │
//   with 'A' -- only the trailing     │  3  │   2  │ tracking num      │
//   4-byte MPID differs, and we do    │  5  │   6  │ timestamp         │
//   not carry it. 'H' halts/resumes   │ 11  │   8  │ stock (padded)    │
//   the symbol via its trading state. │ 19  │   1  │ trading state     │
//                                     │ 20  │   5  │ reserved + reason │
//                                     └─────┴──────┴───────────────────┘
std::size_t ItchBodyBytes(ItchType type) noexcept {
  // The book-moving / trade-gating types we decode. Everything else returns 0,
  // which DecodeItch reports as UNKNOWN_TYPE -- a clean skip, since the
  // MoldUDP64 length prefix already says how far to advance. Widen this switch
  // (and DecodeItch) to add more types
  switch (type) {
    case ItchType::SYSTEM_EVENT:
      return 12;
    case ItchType::TRADING_ACTION:
      return 25;
    case ItchType::ADD_ORDER:
      return 36;
    case ItchType::ADD_ORDER_MPID:
      return 40;  // 'A' plus a trailing 4-byte MPID
    case ItchType::ORDER_DELETE:
      return 19;
    default:
      return 0;
  }
}

// bytes[0] is the type byte, `len` is the length the framing layer said this
// message has. We check before we read anything:
//
//   len == 0                     -> TRUNCATED       (no type byte at all)
//   type we don't decode         -> UNKNOWN_TYPE    (fine, just skip it)
//   len < what the type needs    -> TRUNCATED       (short, can't read it)
//   len > what the type needs    -> LENGTH_MISMATCH (framing disagrees)
//   otherwise                    -> read the fields, OK
DecodeStatus DecodeItch(const std::byte* bytes, std::size_t len,
                        ItchMessage& out) noexcept {
  if (len == 0) {
    return DecodeStatus::TRUNCATED;  // not even a type byte
  }

  const auto type{static_cast<ItchType>(ReadChar(bytes))};
  const std::size_t expected{ItchBodyBytes(type)};
  if (expected == 0) {
    return DecodeStatus::UNKNOWN_TYPE;  // well-framed, just not one we decode
  }
  if (len < expected) {
    return DecodeStatus::TRUNCATED;  // fewer bytes than the type demands
  }
  if (len > expected) {
    return DecodeStatus::LENGTH_MISMATCH;  // framing disagrees with the type
  }

  out = ItchMessage{};
  out.type = type;
  out.stock_locate = ReadU16(bytes + 1);
  out.tracking_number = ReadU16(bytes + 3);
  out.timestamp = ReadU48(bytes + 5);

  switch (type) {
    case ItchType::SYSTEM_EVENT:
      out.event_code = ReadChar(bytes + 11);
      break;

    case ItchType::TRADING_ACTION:
      std::memcpy(out.stock.data(), bytes + 11, out.stock.size());
      out.trading_state = ReadChar(bytes + 19);  // 'H' halted, 'T' trading, ...
      break;

    case ItchType::ADD_ORDER:
    case ItchType::ADD_ORDER_MPID:
      // Identical for the first 36 bytes; 'F' just carries a trailing MPID we
      // do not keep, so both decode the same way here
      out.reference = ReadU64(bytes + 11);
      out.side = (ReadChar(bytes + 19) == 'B') ? Side::BID : Side::ASK;
      out.shares = ReadU32(bytes + 20);
      std::memcpy(out.stock.data(), bytes + 24, out.stock.size());
      out.price = ReadU32(bytes + 32);
      break;

    case ItchType::ORDER_DELETE:
      out.reference = ReadU64(bytes + 11);
      break;

    default:
      return DecodeStatus::UNKNOWN_TYPE;  // unreachable: expected > 0 gates this
  }

  return DecodeStatus::OK;
}

// <---- MoldUDP64 framing ---->

namespace {

// Every declared message (2-byte length prefix + body) must fit inside the
// datagram. Returns false on the first overrun so the whole datagram can be
// dropped rather than half-parsed
bool FramingValid(const std::byte* datagram, std::size_t bytes,
                  std::uint16_t count) noexcept {
  const std::byte* p{datagram + k_mold_header_bytes};
  const std::byte* const stop{datagram + bytes};
  for (std::size_t i{0}; i < count; ++i) {
    const auto before{static_cast<std::size_t>(stop - p)};
    if (before < k_mold_length_prefix_bytes) {
      return false;
    }
    const std::size_t msg_len{ReadU16(p)};  // widen so comparisons stay unsigned
    p += k_mold_length_prefix_bytes;
    const auto after{static_cast<std::size_t>(stop - p)};
    if (after < msg_len) {
      return false;
    }
    p += msg_len;
  }
  return true;
}

// Walk `count` length-prefixed messages starting at `body`, skipping the first
// `skip` (already-seen duplicates), decoding and pushing the rest to the sink.
// Assumes framing was already validated. Returns messages delivered
std::size_t Deliver(ItchSink& sink, const std::byte* body, std::uint16_t count,
                    std::size_t skip) noexcept {
  const std::byte* p{body};
  std::size_t delivered{0};
  for (std::size_t i{0}; i < count; ++i) {
    const std::uint16_t msg_len{ReadU16(p)};
    p += k_mold_length_prefix_bytes;
    if (i >= skip) {
      ItchMessage msg{};
      if (DecodeItch(p, msg_len, msg) == DecodeStatus::OK) {
        sink.OnMessage(msg);
        ++delivered;
      }
      // Unknown/truncated/mismatched single messages are skipped, not fatal:
      // the length prefix already told us how far to advance
    }
    p += msg_len;
  }
  return delivered;
}

}  // namespace

MoldSession::MoldSession(ItchSink& sink) noexcept : m_sink(sink) {}

bool MoldSession::ParseHeader(const std::byte* datagram, std::size_t bytes,
                              MoldHeader& out) noexcept {
  if (bytes < k_mold_header_bytes) {
    return false;
  }
  std::memcpy(out.session.data(), datagram, k_mold_session_bytes);
  out.sequence = ReadU64(datagram + k_mold_session_bytes);          // offset 10
  out.message_count = ReadU16(datagram + k_mold_session_bytes + 8);  // offset 18
  return true;
}

// Work out how many of this datagram's messages we have already seen, so the
// caller can skip them. The tricky case is a partial overlap -- the datagram
// straddles m_expected because a couple of its messages came in early on the
// other feed:
//
//   datagram seqs:   [ first .............................. end )
//   already seen:    [ first ... m_expected )                        <- skip these
//   new tail:                   [ m_expected ................. end )  <- deliver these
//                                 skip = m_expected - first
//
// The clean cases fall out of the same picture: end <= m_expected means the
// whole thing is old (skip all), first > m_expected means a gap opened up.
std::size_t MoldSession::Reconcile(const MoldHeader& header) noexcept {
  // The datagram carries messages [first, first + count). m_expected is the
  // next sequence we want. Compare arithmetically:
  const SeqNum first{header.sequence};
  const SeqNum end{first + header.message_count};

  if (first > m_expected) {
    // A run never arrived. Report it, then re-anchor on this datagram's edge --
    // the missing messages are gone (no live-stream replay), and everything
    // here is new. The sink decides whether to stall until a Reset()
    ++m_gaps;
    m_sink.OnGap(m_expected, first);
    m_expected = end;
    return 0;  // deliver all
  }
  if (end <= m_expected) {
    // Every message here is one we have already seen (the redundant A/B feed)
    ++m_duplicates;
    return header.message_count;  // skip all
  }
  if (first < m_expected) {
    // Overlaps the boundary: a prefix is duplicate, the tail is new
    ++m_duplicates;
    const auto skip{static_cast<std::size_t>(m_expected - first)};
    m_expected = end;
    return skip;
  }

  // first == m_expected: exactly in order
  m_expected = end;
  return 0;
}

// The whole per-datagram pipeline:
//
//   parse header ──► anchored yet? ──► heartbeat / end-of-session? ──► framing
//                    (first packet     (count 0 or 0xFFFF: no          valid?
//                     sets the stream)  messages to deliver)         (else drop
//                                                                     whole)
//                                                            │
//                                        reconcile sequence ─┘  (gap? dup? skip N)
//                                                            │
//                                              deliver the non-skipped tail
//                                              through DecodeItch -> sink
std::size_t MoldSession::OnDatagram(const std::byte* datagram,
                                    std::size_t bytes) noexcept {
  MoldHeader header{};
  if (!ParseHeader(datagram, bytes, header)) {
    ++m_malformed;
    return 0;
  }

  if (!m_anchored) {
    // First datagram sets the stream: adopt its sequence with no gap raised
    m_session = header.session;
    m_expected = header.sequence;
    m_anchored = true;
  }

  if (header.message_count == k_mold_end_of_session) {
    m_anchored = false;  // stream closed for the day
    return 0;
  }
  if (header.message_count == 0) {
    // Heartbeat: no messages, but its sequence still exposes a gap
    if (header.sequence > m_expected) {
      ++m_gaps;
      m_sink.OnGap(m_expected, header.sequence);
      m_expected = header.sequence;
    }
    return 0;
  }

  // Validate framing before touching sequence state, so a structurally broken
  // datagram is dropped whole and never advances m_expected
  if (!FramingValid(datagram, bytes, header.message_count)) {
    ++m_malformed;
    return 0;
  }

  // Framing is sound: reconcile the sequence, then deliver the non-duplicate
  // tail through the sink
  const std::size_t skip{Reconcile(header)};
  return Deliver(m_sink, datagram + k_mold_header_bytes, header.message_count,
                 skip);
}

SeqNum MoldSession::Expected() const noexcept { return m_expected; }

bool MoldSession::InSession() const noexcept { return m_anchored; }

void MoldSession::Reset(SeqNum sequence) noexcept {
  // Re-anchor after recovery or a mid-day start: the next datagram is accepted
  // at `sequence` with no gap raised
  m_expected = sequence;
  m_anchored = true;
}

std::uint64_t MoldSession::Gaps() const noexcept { return m_gaps; }
std::uint64_t MoldSession::Duplicates() const noexcept { return m_duplicates; }
std::uint64_t MoldSession::Malformed() const noexcept { return m_malformed; }

// <---- Translation into engine commands ---->

ItchTranslator::ItchTranslator(const Config& config) noexcept
    : m_config(config) {}

void ItchTranslator::OnSystemEvent(const ItchMessage& msg) noexcept {
  if (msg.type == ItchType::SYSTEM_EVENT) {
    // 'Q' opens market hours, 'M' ends them; O/S/E/C are system-hour
    // boundaries we do not gate on
    if (msg.event_code == 'Q') {
      m_open = true;
    } else if (msg.event_code == 'M') {
      m_open = false;
    }
  } else if (msg.type == ItchType::TRADING_ACTION) {
    m_halted = (msg.trading_state == 'H');  // 'H' halted, else tradable
  }
}

bool ItchTranslator::Tradable() const noexcept { return m_open && !m_halted; }

bool ItchTranslator::ToTicks(Price itch_price, Price& ticks) const noexcept {
  // ITCH prices are unsigned 1e-4 dollars. Fail rather than truncate onto the
  // wrong level when the price is not a whole number of engine ticks
  if (m_config.itch_per_tick == 0 || itch_price % m_config.itch_per_tick != 0) {
    return false;
  }
  ticks = itch_price / m_config.itch_per_tick;
  return true;
}

ItchTranslator::Result ItchTranslator::TranslateAdd(const ItchMessage& msg,
                                                    OrderCommand& out) noexcept {
  if (msg.stock_locate != m_config.stock_locate) {
    return Result::FILTERED;  // a different symbol; the cheapest filter
  }
  if (m_halted) {
    ++m_rejected;
    return Result::HALTED;
  }

  Price ticks{0};
  if (!ToTicks(msg.price, ticks)) {
    ++m_rejected;
    return Result::NOT_ON_TICK;
  }
  if (ticks < m_config.min_price || ticks > m_config.max_price) {
    ++m_rejected;
    return Result::OUT_OF_BAND;
  }

  // BOOK_REBUILD and SYNTHETIC_FLOW both map an add to a resting NEW for the
  // minimal slice. The ITCH order reference becomes the engine order id, so a
  // later ORDER_DELETE resolves to the same order. Distinct synthetic-flow
  // generation is deferred until more message types land
  out = OrderCommand{
      .type = OrderCommand::Type::NEW,
      .side = msg.side,
      .tif = TimeInForce::DAY,
      .client = m_config.feed_client,
      .id = msg.reference,
      .price = ticks,
      .quantity = msg.shares,
      .ingress_ts = 0,  // stamped by the engine on submit
  };
  return Result::COMMAND;
}

ItchTranslator::Result ItchTranslator::TranslateDelete(
    const ItchMessage& msg, OrderCommand& out) noexcept {
  if (msg.stock_locate != m_config.stock_locate) {
    return Result::FILTERED;
  }

  // A delete carries no price and is never gated by a halt -- an order can
  // always be pulled. The engine cancels by id, so side/price/qty are ignored
  out = OrderCommand{
      .type = OrderCommand::Type::CANCEL,
      .side = msg.side,
      .tif = TimeInForce::DAY,
      .client = m_config.feed_client,
      .id = msg.reference,
      .price = 0,
      .quantity = 0,
      .ingress_ts = 0,
  };
  return Result::COMMAND;
}

ItchTranslator::Result ItchTranslator::TranslateReplace(
    const ItchMessage& msg, OrderCommand& out) noexcept {
  // ORDER_REPLACE ('U') is not in the minimal decoded slice. Reported as
  // UNSUPPORTED until the decoder and this mapping are widened together
  (void)msg;
  (void)out;
  ++m_unsupported;
  return Result::UNSUPPORTED;
}

ItchTranslator::Result ItchTranslator::Translate(const ItchMessage& msg,
                                                 OrderCommand& out) noexcept {
  switch (msg.type) {
    case ItchType::SYSTEM_EVENT:
    case ItchType::TRADING_ACTION:
      OnSystemEvent(msg);
      return Result::FILTERED;  // state only, no book effect
    case ItchType::ADD_ORDER:
    case ItchType::ADD_ORDER_MPID:
      return TranslateAdd(msg, out);
    case ItchType::ORDER_DELETE:
      return TranslateDelete(msg, out);
    case ItchType::ORDER_REPLACE:
      return TranslateReplace(msg, out);
    default:
      return Result::FILTERED;  // no book effect / not decoded
  }
}

std::uint64_t ItchTranslator::Unsupported() const noexcept {
  return m_unsupported;
}
std::uint64_t ItchTranslator::Rejected() const noexcept { return m_rejected; }

}  // namespace lfob
