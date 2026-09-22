#include <lfob/order_wire.hpp>

namespace lfob {

bool DecodeOrder(const WireOrder& wire,
                 ClientId client,
                 OrderCommand& out) noexcept {
  // The enums are contiguous from 0, so a single upper-bound check per field
  // rejects any value that would be an out-of-range (UB) cast
  if (wire.type > static_cast<std::uint8_t>(OrderCommand::Type::REPLACE) ||
      wire.side > static_cast<std::uint8_t>(Side::ASK) ||
      wire.tif > static_cast<std::uint8_t>(TimeInForce::FOK)) {
    return false;
  }

  out = OrderCommand{
      .type = static_cast<OrderCommand::Type>(wire.type),
      .side = static_cast<Side>(wire.side),
      .tif = static_cast<TimeInForce>(wire.tif),
      .client = client,
      .id = wire.id,
      .price = wire.price,
      .quantity = wire.quantity,
      .ingress_ts = 0,  // stamped by the engine on submit
  };
  return true;
}

}  // namespace lfob
