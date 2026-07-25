cmake_minimum_required(VERSION 3.20)
project(lockfree_orderbook)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(lob
    src/order_book.cpp
)

# This makes #include <lob/spsc_queue.hpp> work from any target
target_include_directories(lob PUBLIC include/)

add_executable(bench bench/bench_throughput.cpp)
target_link_libraries(bench PRIVATE lob)

add_executable(tests tests/test_order_book.cpp)
target_link_libraries(tests PRIVATE lob)
