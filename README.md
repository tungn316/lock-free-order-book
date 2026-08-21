# lockfree-orderbook

A high-performance, lock-free limit order book in C++23, designed around a
single-writer matching core with lock-free ingress/egress queues and
wait-free market-data snapshots.

## Design

**Threading model:** one gateway thread produces order events, one matching
thread consumes them, and any number of reader threads observe top-of-book.
This "single-writer principle" removes all locking from the matching path:

- **Ingress/Egress** — bounded MPSC ring buffer and SPMC broadcaster with cache-line-padded
  head/tail indices and cached opposing indices to minimize cross-core
  traffic.
- **Matching core** — single-threaded, so book data structures are plain
  memory: dense price-indexed level arrays with an occupancy bitmap for
  O(1) best-price lookup, and intrusive doubly-linked FIFO queues per
  level for O(1) cancels.
- **Market data out** — best bid/offer published through a seqlock:
  the writer never blocks; readers retry on torn reads.
- **Memory** — node arena implemented as a flat std::vector<Order> eliminates heap allocation on the hot path.

## Complexity

| Operation        | Cost |
|------------------|------|
| Add order        | O(1) amortized |
| Cancel order     | O(1) |
| Match at best    | O(1) per fill |
| BBO snapshot     | wait-free for writer, lock-free for readers |

## Building

```zsh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Requires a C++23 compiler (GCC 13+, Clang 17+).

## Benchmarks

`bench/bench_throughput.cpp` measures end-to-end events/sec through the
ingress queue and matching core, and BBO read latency under contention.
