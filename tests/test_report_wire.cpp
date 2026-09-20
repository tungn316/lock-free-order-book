#include <array>
#include <cstddef>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include "lfob/execution_report.hpp"
#include "lfob/report_wire.hpp"

using namespace lfob;

namespace {

ExecutionReport MakeReport(ExecutionReport::Type type) {
  ExecutionReport r{};
  r.seq = 42;
  r.type = type;
  r.client = 7;
  r.order_id = 1234;
  r.price = 100;
  r.quantity = 50;
  return r;
}

}  // namespace

TEST_CASE("RouteOf classifies each report type", "[report_wire]") {
  CHECK(RouteOf(MakeReport(ExecutionReport::Type::FILL)) == Route::BOTH);
  CHECK(RouteOf(MakeReport(ExecutionReport::Type::TOP_OF_BOOK)) == Route::PUBLIC);
  CHECK(RouteOf(MakeReport(ExecutionReport::Type::ACCEPTED)) == Route::PRIVATE);
  CHECK(RouteOf(MakeReport(ExecutionReport::Type::REJECTED)) == Route::PRIVATE);
  CHECK(RouteOf(MakeReport(ExecutionReport::Type::CANCELLED)) == Route::PRIVATE);
  CHECK(RouteOf(MakeReport(ExecutionReport::Type::REPLACED)) == Route::PRIVATE);
  CHECK(RouteOf(MakeReport(ExecutionReport::Type::GAP_NOTICE)) == Route::PRIVATE);
}

TEST_CASE("EncodeReport writes the whole report when it fits", "[report_wire]") {
  const auto report{MakeReport(ExecutionReport::Type::FILL)};
  std::array<std::byte, sizeof(ExecutionReport)> out{};

  const std::size_t n{EncodeReport(report, out.data(), out.size())};
  REQUIRE(n == sizeof(ExecutionReport));
  CHECK(std::memcmp(out.data(), &report, sizeof(report)) == 0);
}

TEST_CASE("EncodeReport refuses a buffer that is too small", "[report_wire]") {
  const auto report{MakeReport(ExecutionReport::Type::FILL)};
  std::array<std::byte, sizeof(ExecutionReport) - 1> out{};

  CHECK(EncodeReport(report, out.data(), out.size()) == 0);
}

TEST_CASE("EncodeReport round-trips through a decode", "[report_wire]") {
  const auto report{MakeReport(ExecutionReport::Type::ACCEPTED)};
  std::array<std::byte, sizeof(ExecutionReport)> out{};
  REQUIRE(EncodeReport(report, out.data(), out.size()) == sizeof(ExecutionReport));

  ExecutionReport decoded{};
  std::memcpy(&decoded, out.data(), sizeof(decoded));
  CHECK(decoded.seq == report.seq);
  CHECK(decoded.type == report.type);
  CHECK(decoded.client == report.client);
  CHECK(decoded.order_id == report.order_id);
  CHECK(decoded.price == report.price);
  CHECK(decoded.quantity == report.quantity);
}
