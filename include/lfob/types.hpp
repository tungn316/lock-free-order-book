#pragma once
#include <cstdint>

namespace lob {

// Fixed-point price in ticks, makes prices usable as array/map indices
using Price = std::int64_t;

// Order quantity in lots.
using Quantity = std::uint64_t;

// Globally unique order identifier, assigned by the exchange
using OrderId = std::uint64_t;

// Monotonic sequence number for ordering events
using SeqNum = std::uint64_t;

enum class Side : std::uint8_t { Bid, Ask };

// Cache line size used to pad atomics
inline constexpr std::size_t kCacheLine = 64;

} // namespace lob
