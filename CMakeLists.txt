cmake_minimum_required(VERSION 3.20)
project(lockfree_orderbook)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Sanitizers ────────────────────────────────────────────────────────────────
# ThreadSanitizer: configure with -DLFOB_TSAN=ON to race-check the lock-free
# rings, seqlock and matching pipeline. Applied directory-wide (so every target
# below, including tests and benches, is instrumented) and to both the compile
# and link lines, as -fsanitize=thread requires. TSan is ~5-10x slower, needs a
# little optimization to stay usable, and cannot be combined with ASan, so it is
# off by default.
option(LFOB_TSAN "Build with ThreadSanitizer" OFF)
if(LFOB_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -O1 -g)
    add_link_options(-fsanitize=thread)
    message(STATUS "ThreadSanitizer enabled")
endif()


# ── Clang tooling ────────────────────────────────────────────────────────────
find_program(CLANG_TIDY    NAMES clang-tidy-19    clang-tidy    REQUIRED)
find_program(CLANG_FORMAT  NAMES clang-format-19  clang-format  REQUIRED)

# ── Dependencies ─────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

Include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.8.1 # or a later release
)

FetchContent_MakeAvailable(Catch2)

# ── Targets ──────────────────────────────────────────────────────────────────
add_library(lfob
    src/order_book.cpp
    src/matching_engine.cpp
    src/affinity.cpp
    src/outbound_buffer.cpp
    src/report_wire.cpp
    src/order_wire.cpp
    src/itch.cpp
    src/client_session.cpp
    src/output_worker.cpp
    src/client_io.cpp
    src/feed_handler.cpp
    src/gateway.cpp
)

target_include_directories(lfob PUBLIC include/)

add_executable(bench_throughput bench/bench_throughput.cpp)
target_link_libraries(bench_throughput PRIVATE lfob Threads::Threads)

add_executable(bench_latency bench/bench_latency.cpp)
target_link_libraries(bench_latency PRIVATE lfob Threads::Threads)

set_target_properties(lfob PROPERTIES
    CXX_CLANG_TIDY "${CLANG_TIDY};--extra-arg=--std=c++23;--warnings-as-errors=*;--header-filter=^${CMAKE_SOURCE_DIR}/(include|src)/.*"
)

# ── Tests ─────────────────────────────────────────────────────────────────────

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

# Helper: one executable per test file, discovered under its own label.
function(add_lfob_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name}
        PRIVATE lfob Catch2::Catch2WithMain Threads::Threads)
    catch_discover_tests(${name}
        DISCOVERY_MODE PRE_TEST
        TEST_PREFIX "${name}::")
endfunction()

add_lfob_test(test_outbound_buffer)
add_lfob_test(test_report_wire)
add_lfob_test(test_order_wire)
add_lfob_test(test_itch)
add_lfob_test(test_seqlock)
add_lfob_test(test_spmc_ring)
add_lfob_test(test_mpsc_ring)
add_lfob_test(test_node_arena)
add_lfob_test(test_order_index)
add_lfob_test(test_price_level)
add_lfob_test(test_book_side)
add_lfob_test(test_order_book)
add_lfob_test(test_matching_engine)

# ── clang-format target ───────────────────────────────────────────────────────
file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    bench/*.cpp
    tests/*.cpp
    include/*.hpp
    include/*.h
)

add_custom_target(format
    COMMAND ${CLANG_FORMAT} -i --verbose --style=Chromium ${ALL_SOURCES}
    COMMENT "Running clang-format"
)
