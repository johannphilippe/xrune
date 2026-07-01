#pragma once
#include "mpmc_queue.hpp"
#include <thread>
#include <atomic>
#include <vector>
#include <functional>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

namespace xrune {

inline void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

// Flush-to-zero + denormals-are-zero: avoids denormal CPU spikes on decaying
// signals. Set on every real-time thread at loop entry (roadmap challenges).
inline void enable_denormal_flush() {
#if defined(__x86_64__) || defined(_M_X64)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

// A fixed pool of spinning worker threads that drain a shared ready queue.
// Workers only execute while `block_active`; the driving (audio) thread seeds
// the queue, participates in draining, and waits on `tasks_remaining`.
struct worker_pool {
    mpmc_queue queue;
    std::vector<std::thread> threads;
    std::atomic<bool> running{false};
    std::atomic<bool> block_active{false};
    std::atomic<int> tasks_remaining{0};
    std::function<void(uint32_t)> execute;

    explicit worker_pool(size_t capacity_pow2) : queue(capacity_pow2) {}
    ~worker_pool() { stop(); }

    void start(size_t n, std::function<void(uint32_t)> fn) {
        execute = std::move(fn);
        running.store(true, std::memory_order_release);
        threads.reserve(n);
        for (size_t i = 0; i < n; ++i)
            threads.emplace_back([this] { loop(); });
    }

    void loop() {
        enable_denormal_flush();
        while (running.load(std::memory_order_acquire)) {
            if (block_active.load(std::memory_order_acquire)) {
                uint32_t t;
                if (queue.pop(t)) execute(t);
                else cpu_relax();
            } else {
                cpu_relax();
            }
        }
    }

    void stop() {
        if (!running.exchange(false, std::memory_order_acq_rel)) return;
        for (auto& t : threads) if (t.joinable()) t.join();
        threads.clear();
    }

    size_t size() const { return threads.size(); }
};

// Smallest power of two >= n (>= 1).
inline size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

} // namespace xrune
