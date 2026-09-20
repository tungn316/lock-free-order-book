#include <cstddef>
#include <cstdint>
#include <cstdlib>  // mkstemp
#include <vector>

#include <unistd.h>  // read, lseek, close, unlink

#include <catch2/catch_test_macros.hpp>

#include "lfob/outbound_buffer.hpp"

using namespace lfob;

namespace {

// A byte sequence with a recognizable, position-dependent pattern so a
// reassembled flush can be compared exactly against what went in
std::vector<std::byte> Pattern(std::size_t n, std::uint8_t seed) {
  std::vector<std::byte> v(n);
  for (std::size_t i{0}; i < n; ++i) {
    v[i] = static_cast<std::byte>((seed + i) & 0xFF);
  }
  return v;
}

// Flush the whole buffer into a throwaway temp file, then read it back. A
// regular file accepts every byte in one writev, so this exercises FlushTo's
// iovec logic (including the wrapped two-span case) without a live socket
std::vector<std::byte> DrainAll(OutboundBuffer& buf) {
  char path[]{"/tmp/ob_test_XXXXXX"};
  const int fd{::mkstemp(path)};
  REQUIRE(fd >= 0);
  ::unlink(path);  // vanish on close; we only need the fd

  const std::size_t total{buf.Pending()};
  while (buf.Pending() > 0) {
    const std::size_t n{buf.FlushTo(fd)};
    REQUIRE(n > 0);  // a regular file never returns EAGAIN
  }

  REQUIRE(::lseek(fd, 0, SEEK_SET) == 0);
  std::vector<std::byte> out(total);
  const ssize_t r{::read(fd, out.data(), total)};
  ::close(fd);
  REQUIRE(r == static_cast<ssize_t>(total));
  return out;
}

}  // namespace

TEST_CASE("fresh buffer is empty", "[outbound]") {
  OutboundBuffer buf;
  CHECK(buf.Empty());
  CHECK(buf.Pending() == 0);
  CHECK(buf.Free() == k_session_outbound_bytes);
}

TEST_CASE("append accounts for bytes", "[outbound]") {
  OutboundBuffer buf;
  const auto frame{Pattern(100, 1)};
  REQUIRE(buf.TryAppend(frame.data(), frame.size()));

  CHECK_FALSE(buf.Empty());
  CHECK(buf.Pending() == 100);
  CHECK(buf.Free() == k_session_outbound_bytes - 100);
}

TEST_CASE("append is all-or-nothing", "[outbound]") {
  OutboundBuffer buf;

  // Fill to within 32 bytes of capacity
  const auto fill{Pattern(k_session_outbound_bytes - 32, 3)};
  REQUIRE(buf.TryAppend(fill.data(), fill.size()));
  CHECK(buf.Free() == 32);

  // A 64-byte frame does not fit: rejected, and nothing is partially written
  const auto big{Pattern(64, 2)};
  CHECK_FALSE(buf.TryAppend(big.data(), big.size()));
  CHECK(buf.Free() == 32);

  // A frame that fits exactly is accepted
  const auto exact{Pattern(32, 4)};
  REQUIRE(buf.TryAppend(exact.data(), exact.size()));
  CHECK(buf.Free() == 0);
  CHECK(buf.Pending() == k_session_outbound_bytes);
}

TEST_CASE("flush drains bytes and preserves content", "[outbound]") {
  OutboundBuffer buf;
  const auto frame{Pattern(500, 5)};
  REQUIRE(buf.TryAppend(frame.data(), frame.size()));

  const auto out{DrainAll(buf)};
  CHECK(out == frame);
  CHECK(buf.Empty());
  CHECK(buf.Free() == k_session_outbound_bytes);
}

TEST_CASE("flush reassembles a wrapped frame in order", "[outbound]") {
  OutboundBuffer buf;

  // Walk head/tail to 64 bytes short of the end of the ring
  const auto filler{Pattern(k_session_outbound_bytes - 64, 6)};
  REQUIRE(buf.TryAppend(filler.data(), filler.size()));
  (void)DrainAll(buf);
  REQUIRE(buf.Empty());

  // A 128-byte frame now straddles the array boundary (64 before, 64 after)
  const auto spanning{Pattern(128, 7)};
  REQUIRE(buf.TryAppend(spanning.data(), spanning.size()));
  CHECK(buf.Pending() == 128);

  // The two-iovec flush must hand the kernel both halves in the right order
  const auto out{DrainAll(buf)};
  CHECK(out == spanning);
}

TEST_CASE("clear discards pending bytes", "[outbound]") {
  OutboundBuffer buf;
  const auto frame{Pattern(200, 8)};
  REQUIRE(buf.TryAppend(frame.data(), frame.size()));

  buf.Clear();
  CHECK(buf.Empty());
  CHECK(buf.Free() == k_session_outbound_bytes);
}
