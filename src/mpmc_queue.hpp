/*
 * Xrune — a real-time audio engine, graph and instancing system.
 * Copyright (C) 2026 Johann Philippe
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once
#include <atomic>
#include <vector>
#include <cstddef>
#include <cstdint>

namespace xrune {

// Bounded lock-free MPMC queue (Vyukov). Fixed power-of-two capacity, per-cell
// sequence numbers, no allocation after init. Stores uint32_t payloads (slot
// indices). Replaces the earlier unsafe queue (pre_roadmap §8): the sequence
// number makes a produced value visible only once fully written, so a consumer
// never reads a half-published cell.
struct mpmc_queue {
    struct cell_t {
        std::atomic<size_t> seq;
        uint32_t val = 0;
    };

    std::vector<cell_t> buffer;
    size_t mask = 0;
    alignas(64) std::atomic<size_t> enqueue_pos{0};
    alignas(64) std::atomic<size_t> dequeue_pos{0};

    mpmc_queue() = default;
    explicit mpmc_queue(size_t capacity_pow2) { init(capacity_pow2); }

    void init(size_t capacity_pow2) {
        buffer = std::vector<cell_t>(capacity_pow2);
        mask = capacity_pow2 - 1;
        for (size_t i = 0; i < capacity_pow2; ++i)
            buffer[i].seq.store(i, std::memory_order_relaxed);
        enqueue_pos.store(0, std::memory_order_relaxed);
        dequeue_pos.store(0, std::memory_order_relaxed);
    }

    bool push(uint32_t v) {
        cell_t* cell;
        size_t pos = enqueue_pos.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer[pos & mask];
            size_t seq = cell->seq.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (dif == 0) {
                if (enqueue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (dif < 0) {
                return false; // full
            } else {
                pos = enqueue_pos.load(std::memory_order_relaxed);
            }
        }
        cell->val = v;
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool pop(uint32_t& v) {
        cell_t* cell;
        size_t pos = dequeue_pos.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer[pos & mask];
            size_t seq = cell->seq.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (dif == 0) {
                if (dequeue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (dif < 0) {
                return false; // empty
            } else {
                pos = dequeue_pos.load(std::memory_order_relaxed);
            }
        }
        v = cell->val;
        cell->seq.store(pos + mask + 1, std::memory_order_release);
        return true;
    }
};

} // namespace xrune
