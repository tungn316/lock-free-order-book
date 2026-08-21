#include "lfob/order_book.hpp"

namespace lfob {
OrderBook::OrderBook(lfob::Price min_price,
                     lfob::Price max_price,
                     std::size_t max_orders,
                     lfob::ReportSink& sink)
    : m_bids(Side::BID, min_price, max_price),
      m_asks(Side::ASK, min_price, max_price),
      m_arena(max_orders),
      m_sink(sink) {
  m_index.reserve(max_orders);
}

void OrderBook::Apply(const OrderCommand& cmd) noexcept {
  if (cmd.type == OrderCommand::Type::NEW) {
    OrderBook::HandleNew(cmd);
  } else if (cmd.type == OrderCommand::Type::CANCEL) {
    OrderBook::HandleCancel(cmd);
  } else {  // cmd.type == OrderCommand::Type::REPLACE
    OrderBook::HandleReplace(cmd);
  }
}

Price OrderBook::BestBid() const noexcept {
  return m_bids.Empty() ? k_no_price : m_bids.BestPrice();
}

Price OrderBook::BestAsk() const noexcept {
  return m_asks.Empty() ? k_no_price : m_asks.BestPrice();
}

bool OrderBook::Empty() const noexcept {
  return (m_last_bid == k_no_price && m_last_ask == k_no_price);
}

void OrderBook::HandleNew(const OrderCommand& cmd) noexcept {
  if (cmd.quantity == 0) {
    EmitReject(cmd, ExecutionReport::RejectReason::ZERO_QUANTITY);
    return;
  }

  const BookSide& own = (cmd.side == Side::BID) ? m_bids : m_asks;
  if (!own.InRange(cmd.price)) {
    EmitReject(cmd, ExecutionReport::RejectReason::PRICE_OUT_OF_BAND);
    return;
  }

  if (cmd.tif == TimeInForce::FOK &&
      !Fillable(cmd.side, cmd.price, cmd.quantity)) {
    EmitReject(cmd, ExecutionReport::RejectReason::FOK_UNFILLABLE);
    return;
  }

  EmitAck(cmd, ExecutionReport::Type::ACCEPTED, cmd.quantity);

  const Quantity leaves{Match(cmd.side, cmd.price, cmd.quantity, cmd)};

  if (leaves > 0) {
    if (cmd.tif == TimeInForce::DAY) {  // Sit passively in the order book waiting for a future counter party 
      Rest(cmd, leaves);
    }
    else {  // Whole order quantity cannot be filled so cancel
      EmitAck(cmd, ExecutionReport::Type::CANCELLED, leaves);
    }
  }

  MaybeEmitTopOfBook();
}

void OrderBook::HandleCancel(const OrderCommand& cmd) noexcept {
  // Find order
  const auto it = m_index.find(cmd.id);
  if (it == m_index.end()) {
    EmitReject(cmd, ExecutionReport::RejectReason::UNKNOWN_ORDER);
    return;
  }

  // Resolve NodeRef - check generation
  const Order* order{m_arena.Resolve(it->second)};
  if (order == nullptr) {
    EmitReject(cmd, ExecutionReport::RejectReason::UNKNOWN_ORDER);
    return;
  }

  const Quantity leaves{order->remaining};
  Unlink(it->second.idx);
  EmitAck(cmd, ExecutionReport::Type::CANCELLED, leaves);
  MaybeEmitTopOfBook();
}

void OrderBook::HandleReplace(const OrderCommand& cmd) noexcept {
  // find order
  const auto it = m_index.find(cmd.id);
  if (it == m_index.end()) {
    EmitReject(cmd, ExecutionReport::RejectReason::UNKNOWN_ORDER);
    return;
  }

  // resolve NodeRef - check generation
  const Order* order{m_arena.Resolve(it->second)};
  if (order == nullptr) {
    EmitReject(cmd, ExecutionReport::RejectReason::UNKNOWN_ORDER);
    return;
  }

  if (cmd.quantity == 0) {
    EmitReject(cmd, ExecutionReport::RejectReason::ZERO_QUANTITY);
    return;
  }

  const BookSide& own = (cmd.side == Side::BID) ? m_bids : m_asks;
  if (!own.InRange(cmd.price)) {
    EmitReject(cmd, ExecutionReport::RejectReason::PRICE_OUT_OF_BAND);
    return;
  }

  // Cancel + re-insert, basically HandleCancel -> HandleNew
  Unlink(it->second.idx);

  EmitAck(cmd, ExecutionReport::Type::REPLACED, cmd.quantity);

  const Quantity leaves{Match(cmd.side, cmd.price, cmd.quantity, cmd)};

  if (leaves > 0) {
    if (cmd.tif == TimeInForce::DAY) {
      Rest(cmd, leaves);
    } else {
      EmitAck(cmd, ExecutionReport::Type::CANCELLED, leaves);
    }
  }

  MaybeEmitTopOfBook();
}

// Dry run to test if FOK is fillable (simulation)
bool OrderBook::Fillable(Side taker_side,
                         Price limit,
                         Quantity quantity) const noexcept {
  const BookSide& side = (taker_side == Side::BID) ? m_asks : m_bids;
  if (side.Empty()) {
    return false;
  }

  Quantity need{quantity};
  Price best_price{side.BestPrice()};

  // Loop through all nodes at this PrriceLevel
  while (need > 0) {
    const bool crosses = (taker_side == Side::BID) ? (best_price <= limit)
                                                   : (best_price >= limit);
    if (!crosses) {
      break;
    }
    const PriceLevel& level{side.LevelAtIndexConst(side.IndexOf(best_price))};
    const Quantity avail{level.TotalQuantity()};
    need = (avail >= need) ? 0 : (need - avail);
    if (need == 0) {
      break;
    }
    best_price = side.NextWorse(side.IndexOf(best_price));
    if (best_price == k_no_price) {
      break;
    }
  }
  return (need == 0);
}

Quantity OrderBook::Match(Side taker_side,
                          Price limit,
                          Quantity quantity,
                          const OrderCommand& cmd) noexcept {
  BookSide& side = (taker_side == Side::BID) ? m_asks : m_bids;
  Quantity leaves{quantity};

  // Loop through all nodes at this PrriceLevel
  while (leaves > 0) {
    const PriceLevel* best{side.Best()};
    if (best == nullptr) {
      break;
    }
    const Price best_price{side.BestPrice()};
    const bool crosses = (taker_side == Side::BID) ? (best_price <= limit)
                                                   : (best_price >= limit);
    if (!crosses) {
      break;
    }

    // Pop a node off the FIFO PriceLevel
    while (leaves > 0 && !best->Empty()) {
      const NodeIdx head{best->Front()};
      Order& maker{m_arena.Node(head)};
      const Quantity traded{std::min(leaves, maker.remaining)};

      leaves -= traded;  // decrement first so taker_leaves is post-trade

      EmitFill(maker, cmd, best_price, traded, /*taker_leaves=*/leaves);

      maker.remaining -= traded;

      if (maker.remaining == 0) {
        Unlink(head);
      }
    }
  }
  return leaves;
}

void OrderBook::Unlink(NodeIdx node) noexcept {
  const Order& order{m_arena.Node(node)};
  BookSide& side = (order.side == Side::BID) ? m_bids : m_asks;

  const std::uint32_t idx{side.IndexOf(order.price)};
  PriceLevel& level{side.LevelAtIndex(idx)};
  level.Remove(m_arena, node);
  if (level.Empty()) {
    side.OnLevelEmptied(idx);
  }

  m_index.erase(order.id);
  m_arena.Release(node);
}

void OrderBook::Rest(const OrderCommand& cmd, Quantity leaves) noexcept {
  const NodeRef ref{m_arena.Acquire()};
  if (ref.idx == k_null_node) {
    EmitReject(cmd, ExecutionReport::RejectReason::ARENA_EXHAUSTED);
    return;
  }

  BookSide& side = (cmd.side == Side::BID) ? m_bids : m_asks;
  const std::uint32_t idx{side.IndexOf(cmd.price)};

  Order& order{m_arena.Node(ref.idx)};
  order.id = cmd.id;
  order.price = cmd.price;
  order.remaining = leaves;
  order.client = cmd.client;
  order.side = cmd.side;
  order.tif = cmd.tif;

  side.LevelAtIndex(idx).PushBack(m_arena, ref.idx);
  side.MarkOccupied(cmd.price);

  m_index[cmd.id] = ref;
}

void OrderBook::EmitFill(const Order& maker,
                         const OrderCommand& taker,
                         Price price,
                         Quantity quantity,
                         Quantity taker_leaves) noexcept {
  // Maker side
  m_sink.Emit(ExecutionReport{
      .seq = ++m_seq,
      .ingress_ts = taker.ingress_ts,
      .match_ts = 0,
      .type = ExecutionReport::Type::FILL,
      .side = maker.side,
      .reason = ExecutionReport::RejectReason::NONE,
      .client = maker.client,
      .order_id = maker.id,
      .counterparty_id = taker.id,
      .price = price,
      .quantity = quantity,
      .leaves_quantity =
          maker.remaining - quantity,  // maker's remaining after fill
      .bid_price = 0,
      .bid_quantity = 0,
      .ask_price = 0,
      .ask_quantity = 0,
  });

  // Taker side
  m_sink.Emit(ExecutionReport{
      .seq = ++m_seq,
      .ingress_ts = taker.ingress_ts,
      .match_ts = 0,
      .type = ExecutionReport::Type::FILL,
      .side = taker.side,
      .reason = ExecutionReport::RejectReason::NONE,
      .client = taker.client,
      .order_id = taker.id,
      .counterparty_id = maker.id,
      .price = price,
      .quantity = quantity,
      .leaves_quantity = taker_leaves,  // taker's remaining after fill
      .bid_price = 0,
      .bid_quantity = 0,
      .ask_price = 0,
      .ask_quantity = 0,
  });
}

void OrderBook::EmitAck(const OrderCommand& cmd,
                        ExecutionReport::Type type,
                        Quantity leaves) noexcept {
  m_sink.Emit(ExecutionReport{
      .seq = ++m_seq,
      .ingress_ts = cmd.ingress_ts,
      .match_ts = 0,
      .type = type,
      .side = cmd.side,
      .reason = ExecutionReport::RejectReason::NONE,
      .client = cmd.client,
      .order_id = cmd.id,
      .counterparty_id = 0,
      .price = cmd.price,
      .quantity = 0,
      .leaves_quantity = leaves,
      .bid_price = 0,
      .bid_quantity = 0,
      .ask_price = 0,
      .ask_quantity = 0,
  });
}

void OrderBook::EmitReject(const OrderCommand& cmd,
                           ExecutionReport::RejectReason reason) noexcept {
  m_sink.Emit(ExecutionReport{
      .seq = ++m_seq,
      .ingress_ts = cmd.ingress_ts,
      .match_ts = 0,
      .type = ExecutionReport::Type::REJECTED,
      .side = cmd.side,
      .reason = reason,
      .client = cmd.client,
      .order_id = cmd.id,
      .counterparty_id = 0,
      .price = cmd.price,
      .quantity = 0,
      .leaves_quantity = 0,
      .bid_price = 0,
      .bid_quantity = 0,
      .ask_price = 0,
      .ask_quantity = 0,
  });
}

void OrderBook::MaybeEmitTopOfBook() noexcept {
  const Price bid{m_bids.Empty() ? k_no_price : m_bids.BestPrice()};
  const Price ask{m_asks.Empty() ? k_no_price : m_asks.BestPrice()};
  const Quantity bq{m_bids.Empty() ? 0 : m_bids.Best()->TotalQuantity()};
  const Quantity aq{m_asks.Empty() ? 0 : m_asks.Best()->TotalQuantity()};

  if (bid == m_last_bid && ask == m_last_ask && bq == m_last_bid_quantity &&
      aq == m_last_ask_quantity) {
    return;  // BBO unchanged, no report
  }
  m_last_bid = bid;
  m_last_ask = ask;
  m_last_bid_quantity = bq;
  m_last_ask_quantity = aq;

  m_sink.Emit(ExecutionReport{
      .seq = ++m_seq,
      .ingress_ts = 0,
      .match_ts = 0,
      .type = ExecutionReport::Type::TOP_OF_BOOK,
      .side = Side::BID,  // n/a
      .reason = ExecutionReport::RejectReason::NONE,
      .client = 0,
      .order_id = 0,
      .counterparty_id = 0,
      .price = 0,
      .quantity = 0,
      .leaves_quantity = 0,
      .bid_price = bid,
      .bid_quantity = bq,
      .ask_price = ask,
      .ask_quantity = aq,
  });
}

}  // namespace lfob
