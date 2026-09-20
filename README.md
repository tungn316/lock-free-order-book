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
| Throughput | _… orders/sec_ |
| Service latency p50 | 90 ns |
| Service latency p99 | 120 ns |
| Service latency max | 3000 ns |

---

## architecture

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
| Client intake | `ClientIo` | `client_io.{hpp,cpp}` | epoll accept + order parse, 1 thread |
| Egress fan-out | `OutputWorker` ×N | `output_worker.{hpp,cpp}` | drains egress ring → its own sockets, 1 thread each |
| Per-client staging | `ClientSession`, `OutboundBuffer` | `client_session.{hpp,cpp}`, `outbound_buffer.{hpp,cpp}` | 2 threads, disjoint fields, no locks |
| ITCH feed | `FeedHandler`, `MoldSession`, `ItchTranslator` | `feed_handler.{hpp,cpp}`, `itch.{hpp,cpp}` | multicast ingress, own thread + core |
| Facade | `Gateway` | `gateway.{hpp,cpp}` | owns the front end + startup/shutdown order |

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

## Front end: gateway & feed handler

The `Gateway` is everything between the engine and the network. Its one job is
to make sure **no slow client or wedged worker can ever reach back and stall the
book**. It owns three moving parts and the strict order they start and stop in.

```
    ITCH multicast ──► FeedHandler ─┐
                                    ├─► MpscRing ─► matching thread
    client TCP ──────► ClientIo ────┘                    │
                                                         ▼
    client TCP ◄────── OutputWorker×N ◄──────────── SpmcRing
```

**`ClientIo` (ingress).** One thread owns the listening socket and an `epoll`
set covering every client fd. On a readable event it either `accept4`s new
connections or reads a client's fixed-size `WireOrder`s, decodes+validates them
into `OrderCommand`s (stamping the session's own client id, so the wire can
never spoof another), and `SubmitBulk`s the batch. A full ingress ring is a
normal outcome: the overflow is refused and each client owed an `INGRESS_FULL`
reject (which `ClientIo` can't emit itself since egress is single-producer), so it
**injects the reject through the engine**, and the matching thread stamps its
sequence and publishes it like any other report.

**`OutputWorker` (egress).** Several workers each own a broadcast cursor and a
**disjoint** partition of client sessions, so the fan-out needs
no locks. A worker drains the egress ring, routes each report
(`PRIVATE`/`PUBLIC`/`BOTH`), stages the bytes into each session's outbound
buffer, and flushes the sockets with non-blocking `writev`. New clients are
handed over by `ClientIo` through a per-worker SPSC inbox that the worker drains
at the top of every poll, `m_sessions` stays mutated by one thread only.

**`ClientSession` + `OutboundBuffer`.** A session is touched by two threads
(`ClientIo` reads, its worker writes) but they touch **disjoint fields**, the
only shared crossing is one atomic `State`. Its
outbound buffer is a bounded per-client byte ring, and the two write paths carry
**opposite loss policies**:

- **public** market data is *lossy*: a full buffer drops the frame, marks the
  session `GAPPED`, and the worker later re-primes it with a gap notice + a fresh
  BBO snapshot (read wait-free through the seqlock). A lagging client wants the
  *current* book, not a replay.
- **private** fills/acks are *lossless-or-disconnect*: a full buffer moves the
  session to `CLOSING` instead of dropping. A trader that never learns it was
  filled is worse off than one that gets disconnected and reconciles.

**`FeedHandler` (ITCH ingress).** The second way orders reach the engine: a
dedicated thread on its own core pulls NASDAQ's MoldUDP64 multicast with batched
`recvmmsg`, runs it through the three-stage ITCH stack, and submits the results.
The stages are deliberately separable and each unit-tested from a byte buffer:

| Stage | Job | File |
|-------|-----|------|
| `MoldSession` | datagram → in-order messages, arithmetic gap/duplicate detection (the A/B feeds are deduped, not errors) | `itch.{hpp,cpp}` |
| `DecodeItch` | big-endian, unaligned message bytes → flat `ItchMessage` POD | `itch.{hpp,cpp}` |
| `ItchTranslator` | `ItchMessage` → `OrderCommand` for one symbol; symbol/tick/band filtering, halt gating | `itch.{hpp,cpp}` |

A feed **gap** is unrecoverable from the live stream, so `FeedHandler` stops
submitting and flags itself unhealthy rather than replaying orders the real
market has already removed. The decoded slice covers the book-moving types
(`SYSTEM_EVENT`, `TRADING_ACTION`, `ADD_ORDER`/`_MPID`, `ORDER_DELETE`); the
execution/cancel/replace family is deferred behind a priority-preserving reduce
command the engine does not yet have (see Roadmap).

**Lifecycle ordering (why the facade exists).** Start-up is fallible-setup-first,
then threads: workers **register their egress cursors before the engine can
publish** (a consumer that joins late silently misses everything before it),
then the listener binds and the feed joins, then every thread starts — workers
before intake, so nothing is ever produced with no one reading. Shutdown is the
mirror: stop intake first so the engine drains, then retire the workers so they
leave the broadcast gracefully instead of being evicted. Sessions are freed only
after every worker has joined, so a worker's raw session pointer can never
dangle.

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
- **Front end & wire** - `outbound_buffer` (byte-ring wrap, partial flush,
  all-or-nothing framing), `report_wire` / `order_wire` (routing + encode/decode
  round-trips), and `itch` (the decoder, MoldUDP64 framing with the full
  gap/duplicate/heartbeat/end-of-session lifecycle, and the translator's
  filter/tick/band/halt paths — all driven from byte buffers, no socket).

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

The engine and the full front end (gateway + ITCH feed handler) are implemented,
tested, and compile/link as one library. What's left lurking:

- **Priority-preserving reduce** - the one real engine feature still missing.
  `OrderCommand` has only `NEW`/`CANCEL`/`REPLACE`, so ITCH's execution/cancel
  family (`E`/`C`/`X`), which shrinks a resting order's shares *in place* (keeping
  its FIFO queue position), maps to `UNSUPPORTED` today — `REPLACE` would send the
  order to the back of the queue and corrupt time priority. Adding a `REDUCE`
  verb (O(1) decrement via the `OrderIndex`) makes `BOOK_REBUILD` *faithful*:
  without it, resting orders never draw down as the real market executes against
  them, so the mirrored book slowly drifts from NASDAQ's.
- **Runtime session reclaim** - a disconnected client currently leaves a `CLOSED`
  shell (holding its fd + buffer) until `Gateway::Stop()`. Fine for bounded runs;
  a long-lived server needs a worker→`ClientIo` retire queue to free sessions
  mid-run.
- **Real ITCH/OUCH wire formats** - inbound `WireOrder` and outbound reports are
  raw-POD placeholders today, routed through the single `DecodeOrder` /
  `EncodeReport` seams so a real protocol swaps in without touching the pipeline.
- **A runnable front end** - a `main` that boots a `Gateway` end-to-end (the
  benchmarks still drive the engine directly through its ring API).

### Non-goals

Single-symbol, in-process, no persistence. The point of the project is the
concurrency and matching core done correctly and measurably, not a production
exchange.
