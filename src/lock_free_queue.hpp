#pragma once
#include <atomic>
#include <cstddef>

namespace xrune {

template <typename T, size_t capacity = 1024>
struct lock_free_queue {
    struct cell_t {
        std::atomic<size_t> sequence;
        T data;
    };

    cell_t buffer[capacity];
    std::atomic<size_t> enqueue_pos;
    std::atomic<size_t> dequeue_pos;

    lock_free_queue() {
        for (size_t i = 0; i < capacity; ++i) {
            buffer[i].sequence.store(i, std::memory_order_relaxed);
        }
        enqueue_pos.store(0, std::memory_order_relaxed);
        dequeue_pos.store(0, std::memory_order_relaxed);
    }

    bool enqueue(const T& data) {
        cell_t* cell;
        size_t pos = enqueue_pos.load(std::memory_order_relaxed);
        while (true) {
            cell = &buffer[pos % capacity];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = (intptr_t)seq - (intptr_t)pos;
            if (diff == 0) {
                if (enqueue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = enqueue_pos.load(std::memory_order_relaxed);
            }
        }
        cell->data = data;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool dequeue(T& data) {
        cell_t* cell;
        size_t pos = dequeue_pos.load(std::memory_order_relaxed);
        while (true) {
            cell = &buffer[pos % capacity];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);
            if (diff == 0) {
                if (dequeue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = dequeue_pos.load(std::memory_order_relaxed);
            }
        }
        data = cell->data;
        cell->sequence.store(pos + capacity, std::memory_order_release);
        return true;
    }
};

} // namespace xrune
