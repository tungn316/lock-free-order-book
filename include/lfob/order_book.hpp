#ifndef ORDER_BOOK_HPP_
#define ORDER_BOOK_HPP_

#include <cstddef>
#include "book_side.hpp"
#include "execution_report.hpp"
#include "node_arena.hpp"
#include "order_index.hpp"

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
// No atomics, no mutexes, no runtime allocation
// Every link is a 32-bit index into m_arena
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

  // Sole entry point
  void Apply(const OrderCommand& cmd) noexcept;

  // Stamp an externally-built report with the next egress sequence and route it
  // to the sink, exactly like a book-generated report. Lets the engine inject
  // synthetic reports (e.g. an INGRESS_FULL reject for a command that never
  // reached the book) without breaking the strictly-increasing, gapless seq.
  // Matching-thread only, same as Apply -- m_seq is not atomic
  void EmitInjected(ExecutionReport report) noexcept;

  [[nodiscard]] Price BestBid() const noexcept;
  [[nodiscard]] Price BestAsk() const noexcept;
  [[nodiscard]] bool Empty() const noexcept;

 private:
  void HandleNew(const OrderCommand& cmd) noexcept;
  void HandleCancel(const OrderCommand& cmd) noexcept;
  void HandleReplace(const OrderCommand& cmd) noexcept;

  // Sweep the crossing side - returns unfilled remainder
  Quantity Match(Side taker_side,
                 Price limit,
                 Quantity quantity,
                 const OrderCommand& cmd) noexcept;

  // Dry-run pass for FOK - can quantity fill at limit
  [[nodiscard]] bool Fillable(Side taker_side,
                              Price limit,
                              Quantity quantity) const noexcept;

  void Rest(const OrderCommand& cmd, Quantity leaves) noexcept;
  void Unlink(NodeIdx node) noexcept;  // remove + release to arena

  void EmitFill(const Order& maker,
                const OrderCommand& taker,
                Price price,
                Quantity quantity,
                Quantity taker_leaves) noexcept;
  void EmitAck(const OrderCommand& cmd,
               ExecutionReport::Type type,
               Quantity leaves) noexcept;
  void EmitReject(const OrderCommand& cmd,
                  ExecutionReport::RejectReason reason) noexcept;
  void MaybeEmitTopOfBook() noexcept;

  BookSide m_bids;
  BookSide m_asks;
  NodeArena m_arena;

  // OrderId -> NodeRef, allocation-free (see order_index.hpp).
  // Generation in the ref makes a stale cancel a
  // clean UnknownOrder reject instead of a wrong-order cancel
  OrderIndex m_index;

  ReportSink& m_sink;
  SeqNum m_seq{0};
  Price m_last_bid{k_no_price};
  Price m_last_ask{k_no_price};
  Quantity m_last_bid_quantity{0};
  Quantity m_last_ask_quantity{0};
};

}  // namespace lfob

#endif  // ORDER_BOOK_HPP_
