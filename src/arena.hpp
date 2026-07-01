#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <new>
#include <utility>

namespace xrune {

// Bump (linear) allocator over a pre-reserved region.
//
// Phase-0 real-time memory primitive: reserve once on the control thread, then
// hand out aligned blocks with no system allocation. reset() rewinds the whole
// region at once (it does NOT run destructors — store POD-ish state here, which
// is exactly the node-state model the redesign adopts).
struct memory_arena {
    std::vector<std::byte> storage;
    size_t offset = 0;

    memory_arena() = default;
    explicit memory_arena(size_t bytes) { reserve(bytes); }

    void reserve(size_t bytes) {
        storage.assign(bytes, std::byte{0});
        offset = 0;
    }

    // Returns nullptr if the request does not fit (never throws, never grows).
    void* allocate(size_t bytes, size_t align = alignof(std::max_align_t)) {
        if (storage.empty()) return nullptr;
        const uintptr_t base = reinterpret_cast<uintptr_t>(storage.data());
        const uintptr_t cur = base + offset;
        const uintptr_t aligned = (cur + (align - 1)) & ~(static_cast<uintptr_t>(align) - 1);
        const size_t new_offset = (aligned - base) + bytes;
        if (new_offset > storage.size()) return nullptr;
        offset = new_offset;
        return reinterpret_cast<void*>(aligned);
    }

    template <typename T>
    T* allocate_array(size_t count) {
        return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
    }

    // Placement-constructs a T in the arena. Caller is responsible for any
    // required destruction before reset() (avoid non-trivial destructors here).
    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* p = allocate(sizeof(T), alignof(T));
        return p ? new (p) T(std::forward<Args>(args)...) : nullptr;
    }

    void reset() { offset = 0; }
    size_t used() const { return offset; }
    size_t capacity() const { return storage.size(); }
    size_t remaining() const { return storage.size() - offset; }
};

// Fixed-size block pool with a free list, for recycling same-sized objects
// (e.g. graph-instance state blocks) on the control thread. acquire()/release()
// do no system allocation after construction.
struct fixed_block_pool {
    std::vector<std::byte> storage;
    std::vector<void*> free_list;
    size_t block_size = 0;
    size_t block_count = 0;

    fixed_block_pool() = default;
    fixed_block_pool(size_t block_bytes, size_t count,
                     size_t align = alignof(std::max_align_t)) {
        init(block_bytes, count, align);
    }

    void init(size_t block_bytes, size_t count, size_t align = alignof(std::max_align_t)) {
        // Round block size up to alignment so every block stays aligned.
        block_size = (block_bytes + (align - 1)) & ~(align - 1);
        block_count = count;
        // Over-allocate by `align` so we can align the base pointer.
        storage.assign(block_size * count + align, std::byte{0});

        const uintptr_t base = reinterpret_cast<uintptr_t>(storage.data());
        const uintptr_t aligned = (base + (align - 1)) & ~(static_cast<uintptr_t>(align) - 1);

        free_list.clear();
        free_list.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            free_list.push_back(reinterpret_cast<void*>(aligned + i * block_size));
        }
    }

    // Returns nullptr when exhausted.
    void* acquire() {
        if (free_list.empty()) return nullptr;
        void* p = free_list.back();
        free_list.pop_back();
        return p;
    }

    void release(void* p) {
        if (p) free_list.push_back(p);
    }

    size_t available() const { return free_list.size(); }
    size_t in_use() const { return block_count - free_list.size(); }
};

} // namespace xrune
