#ifndef BOOK_SIDE_HPP_
#define BOOK_SIDE_HPP_

#include <cstdint>
#include <vector>
#include "price_level.hpp"
#include "types.hpp"

namespace lfob {

class BookSide {
 public:
  BookSide(Side side, Price min_price, Price max_price)
      : m_side(side), m_min_price(min_price) {
    assert(max_price >= min_price);
    const Price delta = (max_price - min_price) + 1;
    m_levels.resize(delta);
    m_occupancy.resize((delta + 63) / 64);
  }

  PriceLevel& LevelAt(Price price) noexcept {
    return LevelAtIndex(IndexOf(price));
  }
  PriceLevel& LevelAtIndex(std::uint32_t idx) noexcept { return LevelRef(idx); }

  PriceLevel* Best() noexcept {
    if (m_best_idx == k_no_best) {
      return nullptr;
    }
    return &LevelRef(m_best_idx);
  }

  [[nodiscard]] const PriceLevel* Best() const noexcept {
    if (m_best_idx == k_no_best) {
      return nullptr;
    }
    return &LevelRef(m_best_idx);
  }
  [[nodiscard]] Price BestPrice() const noexcept {
    assert(m_best_idx != k_no_best);
    return (m_min_price + static_cast<Price>(m_best_idx));
  }

  void MarkOccupied(Price price) noexcept {
    const std::uint32_t idx = IndexOf(price);
    OccWord(idx >> 6U) |= (1ULL << (idx & 63U));

    if (m_best_idx == k_no_best) {
      m_best_idx = idx;
    } else if (m_side == Side::BID) {
      m_best_idx = std::max(m_best_idx, idx);
    } else if (m_side == Side::ASK) {
      m_best_idx = std::min(m_best_idx, idx);
    }
  }

  void OnLevelEmptied(std::uint32_t idx) noexcept {
    OccWord(idx >> 6U) &= ~(1ULL << (idx & 63U));
    if (idx == m_best_idx) {
      AdvanceBest();
    }
  }

  [[nodiscard]] bool Crosses(Price price) const noexcept {
    if (m_best_idx == k_no_best) {
      return false;
    }
    const Price best = PriceOf(m_best_idx);
    return (m_side == Side::BID) ? (price <= best) : (price >= best);
  }

  [[nodiscard]] bool Empty() const noexcept {
    return (m_best_idx == k_no_best);
  }

  [[nodiscard]] bool InRange(Price price) const noexcept {
    return (price >= m_min_price &&
            price <= m_min_price + static_cast<Price>(m_levels.size() - 1));
  }

  [[nodiscard]] std::uint32_t IndexOf(Price price) const noexcept {
    return static_cast<std::uint32_t>(price - m_min_price);
  }

  [[nodiscard]] Price PriceOf(std::uint32_t idx) const noexcept {
    return (m_min_price + static_cast<Price>(idx));
  }

 private:
  void AdvanceBest() noexcept  // bitmap scan to next live level
  {
    const std::uint32_t start = m_best_idx;

    if (m_side == Side::BID) {
      // idx to particular 64 bit bitmask
      std::uint32_t word = start >> 6U;
      // offset
      const std::uint32_t bit = start & 63U;
      // clear bits greater than or equal to best price (there shouldn't be any
      // bits set higher than our m_best_idx but sanity check) Next best BID
      // price is lower
      std::uint64_t bits =
          OccWord(word) & ((bit == 0) ? 0ULL : ((1ULL << bit) - 1));
      while (true) {
        // found price level above best
        if (bits != 0) {
          m_best_idx =
              (word << 6U) +
              (63U - static_cast<std::uint32_t>(std::countl_zero(bits)));
          return;
        }
        // didn't find any price level on the last word
        if (word == 0) {
          m_best_idx = k_no_best;
          return;
        }
        --word;
        bits = OccWord(word);
      }
    } else {  // Side::ASK
      // idx to particular 64 bit bitmask
      std::uint32_t word = start >> 6U;
      // offset
      const std::uint32_t bit = start & 63U;
      // clear bits lower than or equal to best price (there shouldn't be any
      // bits set lower than our m_best_idx but sanity check) Next best ASK
      // price is higher
      std::uint64_t bits =
          (bit == 63U) ? 0ULL : (OccWord(word) & ~((1ULL << (bit + 1)) - 1));
      const std::uint32_t nwords =
          static_cast<std::uint32_t>(m_occupancy.size());
      while (true) {
        // found price level in current level
        if (bits != 0) {
          m_best_idx =
              (word << 6U) + static_cast<std::uint32_t>(std::countr_zero(bits));
          return;
        }
        // go up a level
        ++word;
        // no price level in the highest level, no ASK orders
        if (word >= nwords) {
          m_best_idx = k_no_best;
          return;
        }
        bits = OccWord(word);
      }
    }
  }

  // ── unchecked accessors ────────────────────────────────────────
  // Indices are always derived from validated prices or internal
  // bitmap state, so bounds checks are redundant. Suppression lives
  // here rather than scattered across the class.

  [[nodiscard]] PriceLevel& LevelRef(std::uint32_t idx) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return m_levels[idx];
  }
  [[nodiscard]] const PriceLevel& LevelRef(std::uint32_t idx) const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return m_levels[idx];
  }
  [[nodiscard]] std::uint64_t& OccWord(std::uint32_t word) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return m_occupancy[word];
  }
  [[nodiscard]] std::uint64_t OccWord(std::uint32_t word) const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return m_occupancy[word];
  }

  Side m_side;
  Price m_min_price;
  std::vector<PriceLevel> m_levels;
  std::vector<std::uint64_t> m_occupancy;
  std::uint32_t m_best_idx{k_no_best};
};

}  // namespace lfob

#endif  // BOOK_SIDE_HPP_
