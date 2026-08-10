#ifndef ORDER_BOOK_HPP_
#define ORDER_BOOK_HPP_

#include <cstddef>
#include <unordered_map>
#include "book_side.hpp"
#include "execution_report.hpp"
#include "node_arena.hpp"

namespace lfob {

// Sink for everything the book produces. Implemented by MatchingEngine.
class ReportSink {
 public:
  ReportSink() = default;
  virtual ~ReportSink() = default;
  
  ReportSink(const ReportSink&) = delete;
  ReportSink(ReportSink&&) = delete;

  ReportSink& operator=(const ReportSink&) = delete;
  ReportSink& operator=(ReportSink&&) = delete;

  virtual void Emit(const ExecutionReport& report) = 0;
};

// Single-threaded limit order book.
// No atomics, no mutexes, no runtime allocation. Every link is a
// 32-bit index into arena_.
class OrderBook {
 public:
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) = delete;
  OrderBook& operator=(OrderBook&&) = delete;

  OrderBook(Price min_price,
            Price max_price,
            std::size_t max_orders,
            ReportSink& sink);
  ~OrderBook() = default;

  // Sole entry point; matching thread only.
  void Apply(const OrderCommand& cmd) noexcept;

  Price BestBid() const noexcept;
  Price BestAsk() const noexcept;
  bool Empty() const noexcept;

 private:
  void HandleNew(const OrderCommand& cmd) noexcept;
  void HandleCancel(const OrderCommand& cmd) noexcept;
  void HandleReplace(const OrderCommand& cmd) noexcept;

  // Sweep the crossing side; returns unfilled remainder.
  Quantity Match(Side taker_side,
                 Price limit,
                 Quantity qty,
                 const OrderCommand& cmd) noexcept;

  // Dry-run pass for FOK: can `qty` fill at `limit` without trading?
  bool Fillable(Side taker_side, Price limit, Quantity qty) const noexcept;

  void Rest(const OrderCommand& cmd, Quantity leaves) noexcept;
  void Unlink(NodeIdx node) noexcept;  // remove + release to arena

  void EmitFill(const Order& maker,
                 const OrderCommand& taker,
                 Price price,
                 Quantity qty) noexcept;
  void EmitAck(const OrderCommand& cmd,
                ExecutionReport::Type type,
                Quantity leaves) noexcept;
  void EmitReject(const OrderCommand& cmd,
                   ExecutionReport::RejectReason reason) noexcept;
  void MaybeEmitTopOfBook() noexcept;

  BookSide m_bids;
  BookSide m_asks;
  NodeArena m_arena;

  // OrderId -> NodeRef. Generation in the ref makes a stale cancel a
  // clean UnknownOrder reject instead of a wrong-order cancel.
  std::unordered_map<OrderId, NodeRef> m_index;

  ReportSink& m_sink;
  SeqNum m_seq;
  Price m_last_bid;
  Price m_last_ask;
  Quantity m_last_bid_qty;
  Quantity m_last_ask_qty;
};

}  // namespace lfob

#endif // ORDER_BOOK_HPP_
