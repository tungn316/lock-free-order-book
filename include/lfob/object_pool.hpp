#pragma once
#include <atomic>
#include <cstddef>

namespace lob {

// Lock-free fixed-capacity object pool (Treiber stack freelist).
// Avoids malloc/free on the hot path — allocation is a single CAS.
// Uses a tagged pointer (pointer + generation counter) to prevent
// the ABA problem on the freelist head.
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t capacity);
    ~ObjectPool();

    // Pop a node from the freelist; nullptr if exhausted.
    T* allocate();

    // Push a node back onto the freelist.
    void release(T* obj);

private:
    struct Node {
        T value;
        Node* next; // freelist link
    };

    // Packed {pointer, tag} to make CAS ABA-safe.
    struct TaggedPtr {
        Node* ptr;
        std::uint64_t tag;
    };

    std::atomic<TaggedPtr> free_head_; // top of the freelist
    Node* storage_;                    // contiguous pre-allocated slab
    std::size_t capacity_;
};

} // namespace lob
