# lock-free-order-book

A low-latency limit order book and matching engine in **C++23**, built around a
**single-writer matching core** fed by lock-free queues and observed through
wait-free market-data snapshots. No locks on the order path; no heap allocation
on the hot path.

One slow client or one wedged output thread must never be able to reach back and stall the book.

### Results

_Measured on Ryzen 5600x, governor=performance, isolcpus set --> 4, 5._

| Metric | Value |
|--------|-------|
| Throughput (NEW, no cross) | _… orders/sec_ |
| Service latency p50 | 90 ns |
| Service latency p99 | 120 ns |
| Service latency max | 3000 ns |

---

---

## architecture
'
```
     ITCH multicast ──► FeedHandler ─┐
                                     ├─► MpscRing ─► matching thread ─► OrderBook
     client TCP ──────► ClientIo ────┘   (ingress)        │
                                                          ▼
     client TCP ◄────── OutputWorker×N ◄──────────── SpmcRing (egress broadcast)
                                                          │
                            any reader ◄── Seqlock<Bbo> ◄─┘  (top-of-book)
```

**single-writer principle**: exactly one thread ever mutates the book.
I/O threads hand it work through a ring; consumers read results through
rings and a seqlock. Because the matching core is the only
writer, its internal data structures are plain single-threaded memory, no
atomics, no CAS, no ABA, and all the concurrency lives at the boundaries

| Stage | Type | File | Concurrency |
|-------|------|------|-------------|
| Ingress queue | `MpscRing<OrderCommand>` | `mpsc_ring.hpp` | many producers → 1 consumer, lock-free |
| Matching core | `OrderBook` | `order_book.{hpp,cpp}` | single-threaded, allocation-free |
| Best bid/offer | `Seqlock<Bbo>` | `seqlock.hpp` | 1 writer, N readers, wait-free writer |
| Egress broadcast | `SpmcRing<ExecutionReport>` | `spmc_ring.hpp` | 1 producer → N consumers, lossless in-process |
| Orchestration | `MatchingEngine` | `matching_engine.{hpp,cpp}` | owns rings, seqlock, pinned thread |
| Front end | `Gateway`, `ItchTranslator` | `gateway.hpp`, `itch.hpp` | network ↔ engine (in progress, see Roadmap) |

---

## two opposite loss policies

Ingress and egress are deliberately opposite because this makes the most sense
to me:

- **Ingress is lossy.** `MpscRing::TryPush` *fails* when the ring is full and the
  client gets an `INGRESS_FULL` reject. Refusing an order is a normal, reportable
  outcome.

- **Egress is lossless inside the process, lossy at the socket.** The
  `SpmcRing` broadcasts every `ExecutionReport` to every registered output worker
  and applies **back-pressure**: the producer will not overwrite a slot until the
  slowest worker has read it. No worker ever misses a report. A client that can't
  keep up gets its *public market data* dropped at the socket and doesn't stall the
  engine

### Bounded-stall eviction (when to kick out a consumer)

Back-pressure is only safe while every consumer is alive. A worker that stops
draining, crashed, wedged in a syscall or descheduled would pin the egress gate
forever. The matching thread is the *only* thing draining ingress,
that stall propagates backward: egress fills → the matching thread blocks in
`Emit` → ingress fills → **every client order is rejected**. One dead output
worker takes down the whole exchange(bad).

`MatchingEngine::PublishOrEvict` spins on a full
egress ring for a grace period (`k_egress_stall_budget_ns`, 2 ms), then
**evicts** whoever is holding the gate and moves on. The budget is sized between
two hard bounds: longer than a healthy worker's worst hiccup (or you evict good
workers on jitter), and short enough that the stall cannot overflow ingress
(`ingress_capacity / peak_rate`).

```
epoch odd   → attached, this cursor gates the producer
epoch even  → detached, this cursor is ignored
```

Every attach/detach bumps the epoch (release), and a consumer re-checks the epoch
*after* copying a batch. A worker evicted mid-copy is guaranteed to observe the
change and drop the possibly-torn bytes on the floor, **silent, never wrong**. It
finds out via `IsOutputWorkerLive()`, calls `RejoinOutputWorker()` to re-attach at
the live edge, and owes its clients a gap notice plus a fresh snapshot.
Recovery is *not* a replay: a worker that fell far enough behind to be evicted wants a fresh snapshot

---

## Data structures & complexity

The matching core is single-threaded, which buys the freedom to use the simplest
structure that gives the right complexity:

| Operation | Cost | How |
|-----------|------|-----|
| Add / rest order | O(1) amortized | dense price-indexed levels + occupancy bitmap for best-price lookup |
| Cancel order | O(1) | `OrderId → NodeRef` index + intrusive doubly-linked FIFO unlink |
| Match at best | O(1) per fill | walk the best level's FIFO, price-time priority |
| BBO snapshot | wait-free writer / lock-free reader | seqlock |

- **`NodeArena`** - orders live in a flat, pre-faulted arena; every link is a
  32-bit index, so there is zero runtime allocation and the working set is dense.
- **`OrderIndex`** - `OrderId → NodeRef{idx, generation}`. The generation makes a
  stale cancel a clean `UNKNOWN_ORDER` reject instead of cancelling a recycled
  slot's new occupant.
- **`BookSide`** - price-indexed level array with an occupancy bitmap, so best
  bid/ask is a bit-scan, not a tree walk.
- **`OrderCommand`** (`NEW`/`CANCEL`/`REPLACE`, TIF `DAY`/`IOC`/`FOK`) and
  **`ExecutionReport`** are both trivially copyable PODs so they live directly in
  ring slots - one report type on one ring means global sequencing is trivially
  correct (`seq` strictly increasing, no gaps).

### Memory-ordering highlights

The concurrency is small, deliberate, and documented at each site:

- **Seqlock** - writer bumps `seq` odd → release fence → byte-wise store → `seq`
  even (release). Readers spin while odd, copy, acquire-fence, and retry unless
  `seq` is unchanged. Payload is stored as `std::atomic<unsigned char>` bytes so
  the concurrent copy is data-race-free under the C++ model for any trivially
  copyable `T`.
- **SPMC ring** - producer publishes with a single release store per batch;
  consumers acquire-load `produced`, copy, then validate (epoch unchanged *and*
  not lapped) before advancing their cursor with a release store that pairs with
  the producer's gate scan.
- **Cache-line discipline** - `m_produced`, the gate cache, per-consumer cursors,
  and payload slots are each `alignas(64)` so the producer's writes never falsely
  share a line with a consumer's reads.

---

## Building

Requires a C++23 compiler (GCC 13+ / Clang 17+).

```zsh
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release
```

### ThreadSanitizer

The lock-free code is validated under TSan

```zsh
cmake -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread"
cmake --build build-tsan
ctest --test-dir build-tsan
```

---

## Testing

Every component has a focused unit test (`tests/test_*.cpp`), run under both the
release and TSan builds:

- **Data structures** - `book_side`, `price_level`, `node_arena`, `order_index`,
  `order_book`: correctness of resting, matching, price-time priority, cancels,
  replaces, and reject reasons.
- **Concurrency primitives** - `seqlock`, `mpsc_ring`, `spmc_ring`: torn-read
  freedom, back-pressure, and, for the SPMC ring, the full eviction / rejoin /
  lapped lifecycle under concurrent producer and consumers.
- **Integration** - `matching_engine`: end-to-end submit → match → drain,
  including the eviction path (a wedged worker is detached within the grace
  period and the engine keeps serving) and rejoin at the live edge.

---

## Benchmarks

Two benchmarks drive the engine directly through its ring API (no gateway, no sockets)

- **`bench_throughput`** - steady-state ingress→match→egress throughput. Workload
  is N resting NEW bids across a price band (nothing crosses, pure insertion
  path), with a background thread continuously draining egress so the lossless
  ring never wedges.
- **`bench_latency`** - ingress→match service-latency distribution. Each report
  carries `ingress_ts` and `match_ts` on the *same* monotonic clock, so the delta
  is skew-free. Load is **paced** to a target rate to keep the queue near-empty:
  this measures service latency, not queueing latency. Push the rate toward the
  throughput ceiling to watch the tail grow.

```zsh
# throughput: bench_throughput [num_orders] [cpu_core]
./build-release/bench_throughput 1000000 2

# latency, with pinning + SCHED_FIFO + run-to-run stability report:
#   run_latency.sh [reps] [samples] [rate] [match_core] [bench_core]
sudo bench/run_latency.sh 10 500000 500000 2 3
```

For a trustworthy tail, isolate the hot cores from the scheduler at boot:
`isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3`. `run_latency.sh` prints the machine's
governor, kernel cmdline, and per-core capacity so each run is self-documenting,
and reports the p50/p99/max spread across repeats

## Roadmap

- **ITCH 5.0 / MoldUDP64 feed handler** - `itch.hpp` declares the wire layer
  (MoldUDP64 framing + gap detection, an unaligned big-endian `DecodeItch`, and an
  `ItchTranslator` that maps the outcome feed to engine commands in either
  `BOOK_REBUILD` or `SYNTHETIC_FLOW` mode).
- **Gateway** - `gateway.hpp` declares the network front end (client TCP sessions,
  per-session outbound staging, report routing PRIVATE/PUBLIC/BOTH). Definitions
  pending.

### Non-goals

Single-symbol, in-process, no persistence, no networking layer beyond the
declared gateway. The point of the project is the concurrency and matching core
done correctly and measurably, not a production exchange.
