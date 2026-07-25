#pragma once
#include <atomic>
#include <vector>
#include "book_side.hpp"
#include "object_pool.hpp"
#include "seqlock.hpp"
#include "spsc_queue.hpp"

namespace lob {

// Fill notification emitted when two orders match.
struct Trade {
    OrderId maker_id;
    OrderId taker_id;
    Price price;
    Quantity qty;
};

// Inbound event from the feed/gateway thread.
struct OrderEvent {
    enum class Type : std::uint8_t { New, Cancel };
    Type type;
    OrderId id;
    Side side;
    Price price;
    Quantity qty;
};

class OrderBook {
public:
    OrderBook(Price min_price, Price max_price, std::size_t max_orders);

    // --- Producer thread (feed/gateway) ---
    // Enqueue an event; returns false on backpressure.
    bool submit(const OrderEvent& event);

    // --- Matching thread (single consumer) ---
    // Drain the ingress queue and apply events; returns events processed.
    std::size_t poll();

    // --- Any reader thread ---
    // Lock-free consistent top-of-book snapshot via seqlock.
    Bbo bbo() const;

private:
    // Match an incoming order against the opposite side, then rest
    // any remainder. Appends fills to trades_out_.
    void handle_new(const OrderEvent& event);

    // O(1) cancel via the id->order index.
    void handle_cancel(OrderId id);

    // Recompute and publish the BBO snapshot after any top-of-book change.
    void publish_bbo();

    BookSide bids_;
    BookSide asks_;
    ObjectPool<Order> order_pool_;         // hot-path allocation
    std::vector<Order*> order_index_;      // OrderId -> Order* for O(1) cancel
    SpscQueue<OrderEvent, 1 << 16> ingress_; // feed -> matcher handoff
    SpscQueue<Trade, 1 << 16> trades_out_;   // matcher -> publisher handoff
    Seqlock<Bbo> bbo_;                     // lock-free BBO for readers
};

} // namespace lob
