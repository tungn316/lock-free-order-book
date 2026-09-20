#ifndef ORDER_WIRE_HPP_
#define ORDER_WIRE_HPP_

#include <cstdint>
#include <type_traits>

#include "types.hpp"

namespace lfob {

// Fixed-size inbound wire record: what a client sends per order. Raw-POD and
// host-order, symmetric with the egress report encoding -- both ends share this
// struct. The enum-valued fields travel as raw bytes so an arbitrary value off
// the wire never becomes an out-of-range (UB) scoped enum; DecodeOrder validates
// them before mapping.
struct WireOrder {
  std::uint8_t type;   // OrderCommand::Type: 0 NEW, 1 CANCEL, 2 REPLACE
  std::uint8_t side;   // Side: 0 BID, 1 ASK
  std::uint8_t tif;    // TimeInForce: 0 DAY, 1 IOC, 2 FOK
  std::uint8_t flags;  // reserved for future use
  OrderId id;
  Price price;
  Quantity quantity;
};

static_assert(std::is_trivially_copyable_v<WireOrder>);

// Validate a wire record and map it to an OrderCommand. `client` is stamped by
// the gateway from the owning session -- the wire can never set it, so a client
// cannot spoof another. ingress_ts is left 0 (the engine stamps it on submit).
// Returns false if any enum-valued field is out of range, which the caller
// treats as a protocol violation.
[[nodiscard]] bool DecodeOrder(const WireOrder& wire, ClientId client,
                               OrderCommand& out) noexcept;

}  // namespace lfob

#endif  // ORDER_WIRE_HPP_
