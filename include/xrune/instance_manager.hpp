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
#include "xrune/core.hpp"
#include "xrune/instance.hpp"
#include <vector>
#include <cstdint>
#include <memory>

namespace xrune {

// Stable reference to a spawned instance. `generation` disambiguates recycled
// slots: a handle is stale once its slot has been reused (or freed), so commands
// carrying an old handle are dropped rather than hitting the wrong voice.
// generation 0 is reserved for the null handle.
struct instance_handle {
    uint32_t slot = 0;
    uint32_t generation = 0;
    bool valid() const { return generation != 0; }
    bool operator==(const instance_handle& o) const {
        return slot == o.slot && generation == o.generation;
    }
};
inline constexpr instance_handle null_handle{0, 0};

enum class lifetime_kind : uint8_t {
    permanent,       // lives until explicitly killed
    timed,           // reaped after ttl_blocks processed blocks
    until_finished,  // reaped when the instance sets finished_flag
    until_silent     // reaped after silence_blocks consecutive near-silent blocks
};

struct lifetime_policy {
    lifetime_kind kind = lifetime_kind::permanent;
    size_t ttl_blocks = 0;
    sample_t silence_threshold = 1e-5;
    size_t silence_blocks = 0;
};

struct instance_slot {
    std::unique_ptr<graph_instance> inst; // control thread owns the lifetime
    uint32_t generation = 0;              // control writes (only while inactive)
    lifetime_policy life;
    size_t age_blocks = 0;                // audio thread (while active)
    size_t silent_run = 0;                // audio thread (while active)
};

// Owns the fixed pool of instance slots. All heap allocation/free happens here
// on the CONTROL thread (create/recycle). The audio thread never touches these
// structures except to read an instance pointer for a slot it is *currently*
// processing (ownership is handed off via the engine's command/telemetry
// queues, so control never mutates an active slot). is_valid()/get() are
// control-side queries only — the audio thread validates via its own
// active-generation table (see engine).
struct instance_manager {
    std::vector<instance_slot> slots;
    std::vector<uint32_t> free_slots;
    uint32_t next_generation = 1; // never 0

    void init(size_t max_instances) {
        slots.clear();
        slots.resize(max_instances);
        free_slots.clear();
        free_slots.reserve(max_instances);
        // Push in reverse so early spawns take the low slot indices.
        for (size_t i = max_instances; i-- > 0; )
            free_slots.push_back(static_cast<uint32_t>(i));
        next_generation = 1;
    }

    size_t capacity() const { return slots.size(); }
    size_t available() const { return free_slots.size(); }
    size_t in_use() const { return slots.size() - free_slots.size(); }

    // Control thread: build an instance into a free slot. null_handle if full.
    instance_handle create(const compiled_schedule& sched, size_t sample_rate,
                           const lifetime_policy& life) {
        if (free_slots.empty()) return null_handle;
        auto inst = instantiate(sched, sample_rate);
        if (!inst) return null_handle;

        uint32_t idx = free_slots.back();
        free_slots.pop_back();

        instance_slot& s = slots[idx];
        s.inst = std::move(inst);
        s.generation = next_generation++;
        if (next_generation == 0) next_generation = 1; // skip 0 on wrap
        s.life = life;
        s.age_blocks = 0;
        s.silent_run = 0;
        return instance_handle{idx, s.generation};
    }

    // Control thread: destroy the instance and return the slot to the pool.
    void recycle(uint32_t idx) {
        if (idx >= slots.size()) return;
        slots[idx].inst.reset(); // frees the arena on the control thread
        free_slots.push_back(idx);
    }

    // Control-side validity query.
    bool is_valid(instance_handle h) const {
        return h.valid() && h.slot < slots.size() &&
               slots[h.slot].generation == h.generation &&
               slots[h.slot].inst != nullptr;
    }

    graph_instance* get(instance_handle h) {
        return is_valid(h) ? slots[h.slot].inst.get() : nullptr;
    }

    graph_instance* instance_at(uint32_t slot) {
        return (slot < slots.size()) ? slots[slot].inst.get() : nullptr;
    }
};

} // namespace xrune
