#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "lfob/order_wire.hpp"
#include "lfob/types.hpp"

using namespace lfob;

namespace {

WireOrder MakeWire() {
  return WireOrder{
      .type = static_cast<std::uint8_t>(OrderCommand::Type::NEW),
      .side = static_cast<std::uint8_t>(Side::BID),
      .tif = static_cast<std::uint8_t>(TimeInForce::DAY),
      .flags = 0,
      .id = 4242,
      .price = 10000,
      .quantity = 25,
  };
}

}  // namespace

TEST_CASE("DecodeOrder maps a valid record and stamps the client",
          "[order_wire]") {
  const WireOrder wire{MakeWire()};
  OrderCommand cmd{};

  REQUIRE(DecodeOrder(wire, 77, cmd));
  CHECK(cmd.type == OrderCommand::Type::NEW);
  CHECK(cmd.side == Side::BID);
  CHECK(cmd.tif == TimeInForce::DAY);
  CHECK(cmd.id == 4242);
  CHECK(cmd.price == 10000);
  CHECK(cmd.quantity == 25);
  CHECK(cmd.client == 77);     // stamped by the gateway, not from the wire
  CHECK(cmd.ingress_ts == 0);  // the engine stamps this on submit
}

TEST_CASE("DecodeOrder accepts every in-range enum value", "[order_wire]") {
  OrderCommand cmd{};
  for (std::uint8_t t{0};
       t <= static_cast<std::uint8_t>(OrderCommand::Type::REPLACE); ++t) {
    for (std::uint8_t s{0}; s <= static_cast<std::uint8_t>(Side::ASK); ++s) {
      for (std::uint8_t f{0}; f <= static_cast<std::uint8_t>(TimeInForce::FOK);
           ++f) {
        WireOrder wire{MakeWire()};
        wire.type = t;
        wire.side = s;
        wire.tif = f;
        CHECK(DecodeOrder(wire, 1, cmd));
      }
    }
  }
}

TEST_CASE("DecodeOrder rejects an out-of-range type", "[order_wire]") {
  WireOrder wire{MakeWire()};
  wire.type = 9;  // no such OrderCommand::Type
  OrderCommand cmd{};
  CHECK_FALSE(DecodeOrder(wire, 1, cmd));
}

TEST_CASE("DecodeOrder rejects an out-of-range side", "[order_wire]") {
  WireOrder wire{MakeWire()};
  wire.side = 2;  // only BID(0)/ASK(1)
  OrderCommand cmd{};
  CHECK_FALSE(DecodeOrder(wire, 1, cmd));
}

TEST_CASE("DecodeOrder rejects an out-of-range tif", "[order_wire]") {
  WireOrder wire{MakeWire()};
  wire.tif = 5;  // only DAY(0)/IOC(1)/FOK(2)
  OrderCommand cmd{};
  CHECK_FALSE(DecodeOrder(wire, 1, cmd));
}
