#pragma once
#include <cstddef>
#include <vector>
#include "price_level.hpp"

namespace lob {

// One side of the book. Price levels stored in a dense array indexed
// by tick offset from a base price — O(1) level lookup, cache-friendly,
// no tree rebalancing. A bitmap accelerates finding the next non-empty
// level after the best level is exhausted.
class BookSide {
public:
    BookSide(Side side, Price min_price, Price max_price);

    // Get (or lazily create) the level for a price.
    PriceLevel& level_at(Price price);

    // Best price currently resting on this side; nullptr if empty.
    PriceLevel* best();

    // Called after best() empties to advance the cached best index
    // via the occupancy bitmap.
    void on_level_emptied(Price price);

    bool empty() const;

private:
    Side side_;
    Price min_price_;                 // base of the dense index
    std::vector<PriceLevel> levels_;  // dense array of levels
    std::vector<std::uint64_t> occupancy_; // bitmap of non-empty levels
    std::size_t best_idx_;            // cached index of best level
};

} // namespace lob
