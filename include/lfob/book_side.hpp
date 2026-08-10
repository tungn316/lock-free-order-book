#ifndef BOOK_SIDE_HPP_
#define BOOK_SIDE_HPP_

#include <cstdint>
#include <vector>
#include "price_level.hpp"

namespace lfob {

class BookSide {
 public:
  BookSide(Side side, Price min_price, Price max_price);

  PriceLevel& LevelAt(Price price) noexcept;
  PriceLevel& LevelAtIndex(std::uint32_t idx) noexcept;

  PriceLevel* Best() noexcept;
  [[nodiscard]] const PriceLevel* Best() const noexcept;
  [[nodiscard]] Price BestPrice() const noexcept;

  void MarkOccupied(Price price) noexcept;
  void OnLevelEmptied(std::uint32_t idx) noexcept;

  [[nodiscard]] bool Crosses(Price price) const noexcept;
  [[nodiscard]] bool Empty() const noexcept;
  [[nodiscard]] bool InRange(Price price) const noexcept;

  [[nodiscard]] std::uint32_t IndexOf(Price price) const noexcept;
  [[nodiscard]] Price PriceOf(std::uint32_t idx) const noexcept;

 private:
  void AdvanceBest() noexcept;  // bitmap scan to next live level

  Side m_side;
  Price m_min_price;
  std::vector<PriceLevel> m_levels;
  std::vector<std::uint64_t> m_occupancy;
  std::uint32_t m_best_idx;
};

}  // namespace lfob

#endif // BOOK_SIDE_HPP_
