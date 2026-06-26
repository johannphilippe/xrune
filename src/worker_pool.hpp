#pragma once
#include "core.hpp"
#include "mpmc_queue.hpp"
#include <vector>
#include <thread>
#include <atomic>
#include <functional>

// Include x86 intrinsics at file scope
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace xrune {

// Worker thread pool for parallel graph execution
struct worker_pool {
    std::vector<std::thread> workers;
    mpmc_queue ready_queue;
    std::atomic<bool> running{false};
    std::atomic<size_t> active_workers{0};
    const size_t num_workers;

    worker_pool(size_t count = 0) 
        : num_workers(count > 0 ? count : std::thread::hardware_concurrency()),
          ready_queue(1024) {
    }

    ~worker_pool() {
        stop();
    }

    // Setup CPU denormal flags for real-time safety
    static void setup_rt_safety() {
        #if defined(__x86_64__) || defined(_M_X64)
            _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
            // Note: _MM_SET_DENORMALS_ZERO_MODE may not be available on all compilers
            // This is handled at the OS level or via compiler flags in production
        #endif
    }

    // Real-time safe pause/yield
    static void rt_yield() {
        #if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
        #else
            std::this_thread::yield();
        #endif
    }

    // Start the worker threads
    bool start() {
        if (running.load(std::memory_order_acquire)) {
            return true;
        }

        running.store(true, std::memory_order_release);
        workers.resize(num_workers);

        for (size_t i = 0; i < num_workers; ++i) {
            workers[i] = std::thread([this, i]() {
                setup_rt_safety();
                worker_loop(i);
            });
        }

        return true;
    }

    // Stop the worker threads
    void stop() {
        if (!running.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        // Wait for all workers to finish
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers.clear();
    }

    // Worker thread main loop
    void worker_loop(size_t worker_id) {
        while (running.load(std::memory_order_acquire)) {
            executable_task* task = ready_queue.try_dequeue();
            if (task) {
                active_workers.fetch_add(1, std::memory_order_relaxed);
                task->execute(ready_queue);
                active_workers.fetch_sub(1, std::memory_order_relaxed);
            } else {
                rt_yield();
            }
        }
    }

    // Get the ready queue for task submission
    mpmc_queue& get_ready_queue() {
        return ready_queue;
    }

    // Get number of active workers
    size_t get_active_count() const {
        return active_workers.load(std::memory_order_acquire);
    }
};

} // namespace xrune
