#pragma once
#include "types.hpp"

namespace lob {

// A resting order. Doubly-linked so cancellation is O(1) once the
// order is located via the id->order index.
struct Order {
    OrderId id;          // exchange-assigned unique id
    Price price;         // limit price in ticks
    Quantity remaining;       // unfilled quantity
    Side side;           // Bid or Ask
    Order* prev;         // previous order at the same price level (FIFO)
    Order* next;         // next order at the same price level (FIFO)
    void* level;         // back-pointer to owning PriceLevel for O(1) cancel
};

} // namespace lob
