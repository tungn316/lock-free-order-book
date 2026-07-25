#pragma once
#include <atomic>
#include <cstddef>
#include <optional>
#include "types.hpp"

namespace lob {

// Bounded single-producer/single-consumer lock-free ring buffer.
// Used to hand off market-data/order events from the network/feed
// thread to the matching thread without locks. Capacity must be a
// power of two so index wrapping is a cheap bitmask.
template <typename T, std::size_t Capacity>
class SpscQueue {
public:
    // Returns false if the queue is full (backpressure signal).
    bool try_push(const T& item) 
    {
        size_t tail = producer_.tail.load(std::memory_order::relaxed);

        if (tail - producer_.cached_head == Capacity) {
            producer_.cached_head = consumer_.head.load(std::memory_order::acquire);
            if (tail - producer_.cached_head == Capacity)
                return false;
        }

        buffer_[tail & (Capacity - 1)] = item;
        producer_.tail.store(tail + 1, std::memory_order::release);
        return true;
    }

    // Returns nullopt if the queue is empty.
    std::optional<T> try_pop() 
    {
        size_t head = consumer_.head.load(std::memory_order::relaxed);

        if (head == consumer_.cached_tail) {
            consumer_.cached_tail = producer_.tail.load(std::memory_order::acquire);
            if (head == consumer_.cached_tail)
                return false;
        }

        T data = buffer_[head & (Capacity - 1)];
        consumer_.head.store(head + 1, std::memory_order::release);
        return data;
    }

private:
    // producer lives at tail and keeps local copy of head (cached_head)
    struct alignas(kCacheLine) {
        std::atomic<std::size_t> tail{ 0 };
        std::size_t cached_head{ 0 };
    } producer_;

    // consumer lives at head and keeps local copy of tail (cached_tail)
    struct alignas(kCacheLine) {
        std::atomic<std::size_t> head{ 0 };
        std::size_t cached_tail{ 0 };
    } consumer_;

    alignas(kCacheLine) T buffer_[Capacity];
};

} // namespace lob
