cmake_minimum_required(VERSION 3.20)
project(lockfree_orderbook)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


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
)

target_include_directories(lfob PUBLIC include/)

# add_executable(bench bench/bench_throughput.cpp)
# target_link_libraries(bench PRIVATE lfob)

set_target_properties(lfob PROPERTIES
    CXX_CLANG_TIDY "${CLANG_TIDY};--extra-arg=--std=c++23;--warnings-as-errors=*;--header-filter=^${CMAKE_SOURCE_DIR}/(include|src)/.*"
)

# ── Tests ─────────────────────────────────────────────────────────────────────

# add_executable(tests
#     tests/test_seqlock.cpp
#     tests/test_spmc_ring.cpp
#     tests/test_mpsc_ring.cpp
# )
# target_link_libraries(tests PRIVATE lfob Catch2::Catch2WithMain Threads::Threads)

# list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
# include(CTest)
# include(Catch)
# catch_discover_tests(tests DISCOVERY_MODE PRE_TEST)
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

add_lfob_test(test_seqlock)
add_lfob_test(test_spmc_ring)
add_lfob_test(test_mpsc_ring)

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
