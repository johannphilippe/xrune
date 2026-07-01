#include "arena.hpp"
#include "test_util.hpp"
#include <cstdint>

using namespace xrune;

static bool is_aligned(void* p, size_t align) {
    return (reinterpret_cast<uintptr_t>(p) % align) == 0;
}

int main() {
    // --- memory_arena: basic allocation, alignment, exhaustion, reset ---
    XR_RUN("memory_arena");
    {
        memory_arena a(1024);
        XR_CHECK(a.capacity() == 1024);
        XR_CHECK(a.used() == 0);

        int* x = a.allocate_array<int>(10);
        XR_CHECK(x != nullptr);
        XR_CHECK(is_aligned(x, alignof(int)));
        XR_CHECK(a.used() >= 10 * sizeof(int));

        // Write/read to ensure the memory is usable.
        for (int i = 0; i < 10; ++i) x[i] = i * 7;
        for (int i = 0; i < 10; ++i) XR_CHECK(x[i] == i * 7);

        // Over-aligned allocation stays aligned.
        struct alignas(64) Big { double d[4]; };
        Big* b = a.allocate_array<Big>(2);
        XR_CHECK(b != nullptr);
        XR_CHECK(is_aligned(b, 64));

        // Exhaustion returns nullptr rather than growing/throwing.
        void* huge = a.allocate(4096);
        XR_CHECK(huge == nullptr);

        // reset() rewinds.
        a.reset();
        XR_CHECK(a.used() == 0);
        void* y = a.allocate(512);
        XR_CHECK(y != nullptr);
    }

    // --- memory_arena::create placement-constructs ---
    {
        memory_arena a(256);
        struct Pair { int x; double y; Pair(int a, double b) : x(a), y(b) {} };
        Pair* p = a.create<Pair>(3, 1.5);
        XR_CHECK(p != nullptr);
        XR_CHECK(p->x == 3);
        XR_CHECK_NEAR(p->y, 1.5, 1e-12);
    }

    // --- fixed_block_pool: acquire/release/recycle ---
    XR_RUN("fixed_block_pool");
    {
        fixed_block_pool pool(sizeof(double) * 8, 4);
        XR_CHECK(pool.available() == 4);
        XR_CHECK(pool.in_use() == 0);

        void* a0 = pool.acquire();
        void* a1 = pool.acquire();
        void* a2 = pool.acquire();
        void* a3 = pool.acquire();
        XR_CHECK(a0 && a1 && a2 && a3);
        XR_CHECK(a0 != a1 && a1 != a2 && a2 != a3);
        XR_CHECK(pool.in_use() == 4);

        // Exhausted.
        XR_CHECK(pool.acquire() == nullptr);

        // Release one and reacquire -> recycled slot.
        pool.release(a2);
        XR_CHECK(pool.available() == 1);
        void* again = pool.acquire();
        XR_CHECK(again == a2);
        XR_CHECK(pool.available() == 0);
    }

    XR_MAIN_REPORT();
}
