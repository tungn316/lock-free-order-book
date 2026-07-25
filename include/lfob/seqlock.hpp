#pragma once
#include <atomic>
#include "types.hpp"

namespace lob {

// Seqlock guarding a small POD snapshot (e.g. best bid/offer).
// The single writer (matching thread) never blocks; readers on other
// threads retry if they observe a torn write (odd or changed sequence).
template <typename T>
class Seqlock {
public:
    // Writer-only: publish a new value (increments seq to odd, writes,
    // increments to even).
    void store(const T& value);

    // Reader: spin until a consistent snapshot is obtained.
    T load() const;

private:
    std::atomic<SeqNum> seq_; // odd = write in progress
    T value_;                 // guarded payload
};

// Snapshot of the top of book, published via Seqlock for consumers
// (strategy threads, market-data publishers).
struct Bbo {
    Price bid_price;
    Quantity bid_qty;
    Price ask_price;
    Quantity ask_qty;
};

} // namespace lob
