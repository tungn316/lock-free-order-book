#ifndef GATEWAY_CONFIG_HPP_
#define GATEWAY_CONFIG_HPP_

#include <cstddef>
#include <cstdint>

// Shared tuning knobs for the gateway front end (ClientSession, OutputWorker,
// ClientIo, FeedHandler, Gateway). Kept in one place so the sizing and priority
// decisions read together; see gateway.hpp for the architecture overview.

namespace lfob {

inline constexpr std::size_t k_max_output_workers{4};
inline constexpr std::size_t k_max_client_sessions{256};

// Per-worker handoff inbox. ClientIo pushes newly-accepted sessions here and
// the worker drains them into its fan-out set at the top of each Poll, so a
// session set stays mutated only by its owning worker thread. Power of two for
// the ring; sized to the global session cap so a connect storm can't overflow
inline constexpr std::size_t k_worker_handoff_slots{256};

// Per-session inbound staging. Holds the bytes read off a client socket until
// whole WireOrder records can be parsed out; leftover partial records carry to
// the next read. Tiny -- a few KiB of fixed-size orders is plenty of headroom
inline constexpr std::size_t k_session_inbound_bytes{1U << 12U};  // 4 KiB

// Reports pulled from the egress ring per worker iteration. Large enough that
// the ring is drained in a few syscalls under burst, small enough that the
// worker still flushes sockets promptly
inline constexpr std::size_t k_report_drain_batch{256};

// Serialized frames buffered before a flush is forced regardless of batch
inline constexpr std::size_t k_worker_frame_bytes{1U << 16U};

// Output workers sit above the normal class so they keep draining while the
// box is loaded, but strictly below the matching thread: a worker that
// preempted the engine would be starving the very producer it is waiting on
inline constexpr int k_output_rt_priority{60};
inline constexpr int k_feed_rt_priority{70};

// epoll readiness batch, and the wait timeout. The timeout bounds shutdown:
// request_stop() does NOT wake a blocked epoll_wait, so the loop must return
// periodically to observe the stop token
inline constexpr std::size_t k_epoll_max_events{64};
inline constexpr int k_io_poll_timeout_ms{100};

// How many empty reads a worker tolerates before it stops assuming the market
// is quiet and checks whether it is still attached. A liveness check is an
// acquire load on a cold line, so it is cheap, but not free enough to do on
// every empty poll of an idle feed
inline constexpr std::uint32_t k_idle_polls_before_liveness_check{1024};

}  // namespace lfob

#endif  // GATEWAY_CONFIG_HPP_
