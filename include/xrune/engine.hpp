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
#include "xrune/instance_manager.hpp"
#include "xrune/audio/backend.hpp"
#include "xrune/audio/rtaudio_backend.hpp"
#include "xrune/util/worker_pool.hpp"
#include "xrune/util/rt_check.hpp"
#include <readerwriterqueue.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <memory>

namespace xrune {

struct route_target {
    bool to_master = true;
    instance_handle instance = null_handle;
    size_t terminal = 0;

    static route_target master() { return {true, null_handle, 0}; }
    static route_target to(instance_handle h, size_t term = 0) { return {false, h, term}; }
};

enum class command_type { activate, kill, set_parameter, connect, disconnect };

struct command_event {
    command_type type;
    instance_handle handle;
    size_t node_index = 0;
    size_t parameter_index = 0;
    sample_t parameter_value = 0.0;
    size_t src_terminal = 0;
    route_target dest{};
};

struct telemetry_event { uint32_t slot = 0; };

struct active_route {
    uint32_t src_slot = 0, src_gen = 0;
    size_t src_terminal = 0;
    bool to_master = true;
    uint32_t dst_slot = 0, dst_gen = 0;
    size_t dst_terminal = 0;
};

// Audio host + instance graph with a lock-free instance manager and an optional
// worker pool. Instances form a cross-instance routing DAG; the executor runs it
// either single-threaded or in parallel (Phase 6). Parallelism is per-instance:
// each instance is a task, workers + the audio thread drain a Vyukov MPMC ready
// queue, and a `tasks_remaining` atomic is the block barrier. Instances own
// disjoint arenas and the master is summed single-threaded afterwards, so
// parallel output is bit-identical to sequential.
struct engine {
    size_t sample_rate = 48000;
    size_t block_size = 128;
    size_t input_channels = 0;
    size_t output_channels = 2;

    instance_manager mgr;

    std::vector<uint32_t> active_gen;
    std::vector<uint32_t> active_list;
    std::vector<active_route> routes;

    // Master mix buffers, one per output channel. Allocated from an arena at
    // init() (control thread) so each channel starts on a `simd_align` (64-byte)
    // boundary, exactly like an instance's buffer_pool. Each channel gets its own
    // aligned allocation rather than a strided slice of one block, so alignment
    // holds for *any* block size (a strided layout would only be 64-aligned once
    // block_size * sizeof(sample_t) >= 64). Indexing is unchanged:
    // master_buffers[channel][frame].
    memory_arena master_arena;
    sample_t** master_buffers = nullptr;

    moodycamel::ReaderWriterQueue<command_event> command_queue;
    moodycamel::ReaderWriterQueue<telemetry_event> telemetry_queue;

    std::unique_ptr<audio_backend> backend;

    // Parallel execution.
    size_t num_worker_threads = 0;
    std::unique_ptr<worker_pool> workers;
    std::unique_ptr<std::atomic<int>[]> dep;   // per slot: unmet upstream instances
    size_t parallel_nf = 0;

    // Scratch (single-threaded ordering / reaping).
    std::vector<int> indeg_scratch;
    std::vector<uint32_t> order_scratch;
    std::vector<uint32_t> reap_scratch;

    engine() = default;
    ~engine();

    void use_backend(std::unique_ptr<audio_backend> b);

    bool init(size_t sr = 48000, size_t bs = 128, size_t ins = 0, size_t outs = 2,
              size_t max_instances = 128, size_t num_workers = 0);

    bool start();

    void stop();

    // ---- Control-thread API ----

    instance_handle spawn(const compiled_schedule& sched, lifetime_policy life = {},
                          route_target dest = {}, size_t src_terminal = 0);

    void kill(instance_handle h);
    void set_parameter(instance_handle h, size_t node_index, size_t param, sample_t value);
    void connect(instance_handle src, size_t src_terminal, route_target dest);
    void disconnect(instance_handle src, size_t src_terminal, route_target dest);

    size_t reclaim();

    bool is_valid(instance_handle h) const;
    size_t active_count() const;
    size_t route_count() const;

    // ---- Audio thread ----

    int process(double* out_buf, const double* /*in_buf*/, unsigned int n_frames);

private:
    // ---- Executors ----

    void compute_sequential(size_t nf);

    void compute_parallel(size_t nf);

    // A single instance-task: gather inputs, process, release downstream.
    void execute_instance_task(uint32_t slot);

    // Zero input terminals then sum all live incoming routes (deterministic order).
    void gather_inputs(uint32_t slot, graph_instance* g, size_t nf);

    // ---- Command handling ----

    void drain_commands();

    void add_route(const command_event& cmd);

    void remove_route(const command_event& cmd);

    bool route_live(const active_route& r) const;

    void compute_order();

    void reap_pass(size_t nf);

    // ---- Buffer helpers ----

    void zero_input_terminals(graph_instance* g, size_t nf);

    static void sum_terminal_to_instance(graph_instance* src, size_t src_t,
                                         graph_instance* dst, size_t dst_t, size_t nf);

    void sum_terminal_to_master(graph_instance* src, size_t src_t, size_t nf);

    static sample_t terminal_peak(graph_instance* g, size_t nf);

    void release_slot(uint32_t slot);

    static bool should_reap(instance_slot& s, graph_instance* g, sample_t block_peak);
};

} // namespace xrune
