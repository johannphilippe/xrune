#pragma once
#include <atomic>
#include <vector>
#include <memory>

namespace xrune {

// Forward declaration
struct executable_task;

// Very simple MPMC queue using a lock-free ring buffer
// This allows threads to be both producers and consumers
struct mpmc_queue {
    struct Cell {
        std::atomic<executable_task*> task;
        
        Cell() { task.store(nullptr, std::memory_order_relaxed); }
        Cell(const Cell&) = delete;
        Cell& operator=(const Cell&) = delete;
    };

    std::vector<std::unique_ptr<Cell>> cells;
    const size_t capacity;
    std::atomic<size_t> head;
    std::atomic<size_t> tail;

    mpmc_queue(size_t cap = 1024) : capacity(cap), head(0), tail(0) {
        cells.reserve(cap);
        for (size_t i = 0; i < cap; ++i) {
            cells.push_back(std::make_unique<Cell>());
        }
    }

    ~mpmc_queue() = default;

    // Enqueue a task (may fail if queue is full)
    bool try_enqueue(executable_task* task) {
        size_t current_tail = tail.load(std::memory_order_relaxed);
        size_t current_head = head.load(std::memory_order_acquire);
        
        // Check if queue is full
        if ((current_tail + 1) % capacity == current_head) {
            return false;
        }
        
        // Try to reserve the slot
        size_t new_tail = (current_tail + 1) % capacity;
        if (!tail.compare_exchange_strong(current_tail, new_tail, 
                                          std::memory_order_acq_rel)) {
            return false;
        }
        
        // Store the task
        cells[current_tail]->task.store(task, std::memory_order_release);
        return true;
    }

    // Dequeue a task (returns nullptr if queue is empty)
    executable_task* try_dequeue() {
        size_t current_head = head.load(std::memory_order_relaxed);
        size_t current_tail = tail.load(std::memory_order_acquire);
        
        // Check if queue is empty
        if (current_head == current_tail) {
            return nullptr;
        }
        
        // Try to reserve the slot
        size_t new_head = (current_head + 1) % capacity;
        if (!head.compare_exchange_strong(current_head, new_head,
                                          std::memory_order_acq_rel)) {
            return nullptr;
        }
        
        // Load the task
        executable_task* task = cells[current_head]->task.load(std::memory_order_acquire);
        cells[current_head]->task.store(nullptr, std::memory_order_relaxed);
        
        return task;
    }

    // Check if queue is empty
    bool empty() const {
        return head.load(std::memory_order_acquire) == tail.load(std::memory_order_acquire);
    }
};

} // namespace xrune
