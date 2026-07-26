#pragma once
#include <atomic>
#include <cstddef>
#include <new>

namespace lob {

// Lock-free fixed-capacity object pool (Treiber stack freelist).
// Avoids malloc/free on the hot path — allocation is a single CAS.
// Uses a tagged pointer (pointer + generation counter) to prevent
// the ABA problem on the freelist head.
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t capacity) : 
        capacity_(capacity),
        storage_(static_cast<Node*>(::operator new(capacity * sizeof(Node), std::align_val_t{alignof(Node)})))
    {
        for (auto i{0uz}; i < capacity_ - 1; ++i) {
            storage_[i].next = &storage_[i + 1];
        }
        storage_[capacity_ - 1].next = nullptr;
        for (auto i{0uz}; i < capacity_; ++i) {
            new (&storage_[i]) Node{};
        }
        free_head_.store({storage_, 0}, std::memory_order::relaxed);
    }

    ~ObjectPool()
    {
        for (auto i{0uz}; i < capacity_; ++i) {
            storage_[i].~Node();
        }
        ::operator delete(storage_, std::align_val_t{alignof(Node)});
    }

    // Pop a node from the freelist; nullptr if exhausted.
    T* allocate() 
    {
        TaggedPtr old_head = free_head_.load(std::memory_order::acquire);
        while (old_head.ptr != nullptr) {
            TaggedPtr new_head {
                old_head.ptr->next,
                    old_head.tag + 1
            };
            if (free_head_.compare_exchange_weak(
                        old_head, new_head,
                        std::memory_order::acquire,
                        std::memory_order::acquire
                        ))
            {
                return &old_head.ptr->value;
            }
        }
        return nullptr;
    }

    // Push a node back onto the freelist.
    void release(T* obj)
    {
        Node* node = reinterpret_cast<Node&>(obj);

        TaggedPtr old_head = free_head_.load(std::memory_order::relaxed);
        TaggedPtr new_head;
        do {
            node->next = old_head.ptr;
            new_head = {node, old_head.tag + 1};
        } while (!free_head_.compare_exchange_weak(
                    old_head, new_head,
                    std::memory_order::release,
                    std::memory_order::relaxed
                    ));
    }

private:
    struct Node {
        T value;
        Node* next; // freelist link
    };

    // Packed {pointer, tag} to make CAS ABA-safe.
    struct alignas(16) TaggedPtr {
        Node* ptr;
        std::uint64_t tag;
    };

    static_assert(std::atomic<TaggedPtr>::is_always_lock_free, "TaggedPtr CAS is not lock-free on this platform");

    std::atomic<TaggedPtr> free_head_; // top of the freelist
    Node* storage_;                    // contiguous pre-allocated slab
    std::size_t capacity_;
};

} // namespace lob
