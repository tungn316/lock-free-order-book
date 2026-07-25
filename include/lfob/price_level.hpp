#pragma once
#include "order.hpp"

namespace lob {

// All orders resting at a single price, in time priority (FIFO).
// Only ever mutated by the single matching thread, so it's a plain
// intrusive doubly-linked list — no atomics needed here.
class PriceLevel {
public:
    explicit PriceLevel(Price price);

    void push_back(Order* order);   // add at back (time priority)
    void remove(Order* order);      // O(1) unlink for cancels
    Order* front() const;           // oldest order (match target)

    Price price() const;
    Quantity total_qty() const;          // aggregate size, for depth queries
    bool empty() const;

private:
    Price price_;
    Quantity total_qty_;   // maintained incrementally on add/remove/fill
    Order* head_;     // oldest order
    Order* tail_;     // newest order
};

} // namespace lob
