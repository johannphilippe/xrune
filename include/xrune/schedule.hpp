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
#include "xrune/blueprint.hpp"
#include <vector>
#include <queue>
#include <algorithm>

namespace xrune {

constexpr long SILENT_SLOT = -1;

// Reference to a producer channel (node index + output channel), or silent.
struct src_ref {
    long node = SILENT_SLOT;
    size_t chan = 0;
    bool silent() const { return node == SILENT_SLOT; }
};

// Result of compiling a blueprint (control thread). Immutable; shared by all
// instances. In addition to the single-rate layout it carries the multi-rate
// schedule: each node has a power-of-two region rate and is executed
// `calls == region_rate` times per cycle. Output buffers are sized
// region_rate * base_block per channel; producer/consumer slices are derived at
// runtime from these rates.
struct compiled_schedule {
    const graph_blueprint* bp = nullptr;
    size_t block_size = 0;   // base block size B
    bool ok = false;

    std::vector<size_t> topo_order;

    // Multi-rate.
    std::vector<size_t> region_rate;   // per node: sample-rate multiplier (power of two)
    std::vector<size_t> node_block;    // per node: samples per call (block-size axis)
    std::vector<size_t> calls;         // per node: calls per cycle = (region_rate*B)/node_block
    std::vector<size_t> ctx_base;      // per node: first (node,call) context index
    size_t total_calls = 0;

    // Output buffer layout (sample offsets into the instance buffer pool).
    std::vector<size_t> out_base_sample; // per node: first output sample offset
    std::vector<size_t> out_stride;      // per node: samples per output channel = region_rate*B
    size_t total_output_samples = 0;
    size_t max_stride = 0;               // largest per-channel buffer (for the silent buffer)

    // State layout.
    std::vector<size_t> state_offset;
    std::vector<size_t> state_bytes;
    size_t total_state_bytes = 0;

    // Wiring: per node, per audio input / per control port -> producer channel.
    std::vector<std::vector<src_ref>> input_source;
    std::vector<std::vector<src_ref>> param_source;

    // Control ports.
    std::vector<size_t> param_base;
    size_t total_params = 0;
};

// True for 1,2,4,8,… The engine requires a power-of-two block size: every
// per-channel buffer then also lands on a 64-byte (simd_align) boundary.
bool is_power_of_two(size_t x);

// Compile a blueprint into an execution plan: topological order, per-node call
// counts (multi-rate), buffer assignments and control-port wiring. Done once,
// on the control thread; every instance of the blueprint shares the result.
// Check `.ok` — a cycle or an inconsistent rate/block configuration fails here.
//
// Implementation: src/schedule.cpp
compiled_schedule compile(const graph_blueprint& bp, size_t block_size);

} // namespace xrune
