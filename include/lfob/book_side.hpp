#ifndef BOOK_SIDE_HPP_
#define BOOK_SIDE_HPP_

#include <cstdint>
#include <vector>
#include "price_level.hpp"
#include "types.hpp"

namespace lfob {

class BookSide {
 public:
  // No need to prefault as resize value initialises
  BookSide(Side side, Price min_price, Price max_price)
      : m_side(side), m_min_price(min_price) {
    assert(max_price >= min_price);
    const Price delta{(max_price - min_price) + 1};
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
    return PriceOf(m_best_idx);
  }

  void MarkOccupied(Price price) noexcept {
    const std::uint32_t idx{IndexOf(price)};
    // m_occupancy holds 64 ticks per uint64_t so (idx >> 6U) finds the right bitmask
    // OR the mask with our price so will always mark occupied
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
    // m_occupancy holds 64 ticks per uint64_t so (idx >> 6U) finds the right bitmask
    // AND the mask with the following so we keep everything else the same and always turn
    // our price to 0
    OccWord(idx >> 6U) &= ~(1ULL << (idx & 63U));
    if (idx == m_best_idx) {
      AdvanceBest();
    }
  }

  [[nodiscard]] bool Crosses(Price price) const noexcept {
    if (m_best_idx == k_no_best) {
      return false;
    }
    const Price best{PriceOf(m_best_idx)};
    // For Side::BID, for a cross we need someone to sell for our price or lower
    // We want anything except buying for a higher price
    // For Side::ASK, we are selling for x price so for a cross we need someone to buy for our price or higher
    // We want anything except selling for a lower price
    return (m_side == Side::BID) ? (price <= best) : (price >= best);
  }

  [[nodiscard]] bool Empty() const noexcept {
    return (m_best_idx == k_no_best);
  }

  [[nodiscard]] bool InRange(Price price) const noexcept {
    return (price >= m_min_price &&
            price <= PriceOf(static_cast<std::uint32_t>(m_levels.size() - 1)));
  }

  [[nodiscard]] std::uint32_t IndexOf(Price price) const noexcept {
    return static_cast<std::uint32_t>(price - m_min_price);
  }

  [[nodiscard]] Price PriceOf(std::uint32_t idx) const noexcept {
    return (m_min_price + static_cast<Price>(idx));
  }

  [[nodiscard]] const PriceLevel& LevelAtIndexConst(
      std::uint32_t idx) const noexcept {
    return LevelRef(idx);
  }


  // Returns k_no_best on no next worse idx
  [[nodiscard]] std::uint32_t NextWorseIdx(std::uint32_t from_idx) const noexcept
  {
    if (m_side == Side::BID) {
      if (from_idx == 0) {
        return k_no_best;
      }
      std::uint32_t idx{from_idx - 1};
      std::uint32_t word{idx >> 6U}; // specific uint64_t in m_occupancy that holds this price
      const std::uint32_t bit{idx & 63U}; // offset into the uint64_t
      std::uint64_t bits{OccWord(word) & (bit == 63U ? ~0ULL : ((1ULL << (bit + 1)) - 1))};
      while (true) {
        if (bits != 0) {
          const std::uint32_t hit{(word << 6U) + (63U - static_cast<std::uint32_t>(std::countl_zero(bits)))};
          return hit;
        }
        if (word == 0) {
          return k_no_best;
        }
        --word;
        bits = OccWord(word);
      }
    } else {  // ASK
      const std::uint32_t nwords{static_cast<std::uint32_t>(m_occupancy.size())};
      std::uint32_t idx{from_idx + 1};
      std::uint32_t word{idx >> 6U};
      if (word >= nwords) {
        return k_no_best;
      }
      const std::uint32_t bit{idx & 63U};
      std::uint64_t bits{OccWord(word) & ~((1ULL << bit) - 1)};
      while (true) {
        if (bits != 0) {
          const std::uint32_t hit{(word << 6U) + static_cast<std::uint32_t>(std::countr_zero(bits))};
          return hit;
        }
        ++word;
        if (word >= nwords) {
          return k_no_best;
        }
        bits = OccWord(word);
      }
    }
  }

  // Next occupied price strictly worse than 'from' (lower for BID,
  // higher for ASK). Returns k_no_price when none.
  // Const used by Fillable's dry-run and Match's sweep.
  [[nodiscard]] Price NextWorse(std::uint32_t from_idx) const noexcept
  {
      const std::uint32_t idx{NextWorseIdx(from_idx)};
      return (idx == k_no_best) ? k_no_price : PriceOf(idx);
  }

 private:
  void AdvanceBest() noexcept  // bitmap scan to next live level
  {
      m_best_idx = NextWorseIdx(m_best_idx);
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
