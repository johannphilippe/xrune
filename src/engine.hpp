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
#include "core.hpp"
#include "instance.hpp"
#include "instance_manager.hpp"
#include "audio_backend.hpp"
#include "rtaudio_backend.hpp"
#include "worker_pool.hpp"
#include "rt_check.hpp"
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
    ~engine() { stop(); }

    void use_backend(std::unique_ptr<audio_backend> b) { backend = std::move(b); }

    bool init(size_t sr = 48000, size_t bs = 128, size_t ins = 0, size_t outs = 2,
              size_t max_instances = 128, size_t num_workers = 0) {
        // Block size must be a power of two (required by multi-rate slicing, the
        // FFT nodes, and SIMD per-channel alignment).
        if (!is_power_of_two(bs)) return false;
        sample_rate = sr;
        block_size = bs;
        input_channels = ins;
        output_channels = outs;
        num_worker_threads = num_workers;

        mgr.init(max_instances);
        active_gen.assign(max_instances, 0);
        active_list.clear();       active_list.reserve(max_instances);
        routes.clear();            routes.reserve(max_instances * 4);
        indeg_scratch.assign(max_instances, 0);
        order_scratch.clear();     order_scratch.reserve(max_instances);
        reap_scratch.clear();      reap_scratch.reserve(max_instances);

        dep = std::make_unique<std::atomic<int>[]>(max_instances);
        for (size_t i = 0; i < max_instances; ++i) dep[i].store(0, std::memory_order_relaxed);

        // Pre-size the lock-free queues so the audio thread never grows them.
        // Telemetry is produced on the AUDIO thread (reaps) -> try_enqueue only.
        command_queue = moodycamel::ReaderWriterQueue<command_event>(max_instances * 8 + 64);
        telemetry_queue = moodycamel::ReaderWriterQueue<telemetry_event>(max_instances * 4 + 16);

        // Master buffers: one 64-byte-aligned block per output channel, plus the
        // pointer table, all from one arena reservation. Each channel allocation
        // is rounded up to simd_align, so budget that per channel.
        const size_t chan_bytes = block_size * sizeof(sample_t);
        master_arena.reserve(output_channels * (chan_bytes + simd_align)
                             + output_channels * sizeof(sample_t*) + simd_align);
        master_buffers = master_arena.allocate_array<sample_t*>(output_channels);
        if (!master_buffers) return false;
        for (size_t ch = 0; ch < output_channels; ++ch) {
            master_buffers[ch] = static_cast<sample_t*>(
                master_arena.allocate(chan_bytes, simd_align));
            if (!master_buffers[ch]) return false;
            std::fill(master_buffers[ch], master_buffers[ch] + block_size, 0.0);
        }

        if (!backend) backend = std::make_unique<rtaudio_backend>();

        backend_config cfg{sample_rate, block_size, input_channels, output_channels};
        return backend->open(cfg, [this](double* out, const double* in, unsigned int n) {
            this->process(out, in, n);
        });
    }

    bool start() {
        if (num_worker_threads > 0 && !workers) {
            workers = std::make_unique<worker_pool>(next_pow2(std::max<size_t>(mgr.capacity(), 2)));
            workers->start(num_worker_threads, [this](uint32_t slot) { execute_instance_task(slot); });
        }
        return backend ? backend->start() : false;
    }

    void stop() {
        if (workers) { workers->stop(); workers.reset(); }
        if (backend) { backend->stop(); backend->close(); }
    }

    // ---- Control-thread API ----

    instance_handle spawn(const compiled_schedule& sched, lifetime_policy life = {},
                          route_target dest = {}, size_t src_terminal = 0) {
        reclaim();
        instance_handle h = mgr.create(sched, sample_rate, life);
        if (!h.valid()) return null_handle;
        command_queue.enqueue({command_type::activate, h, 0, 0, 0.0, 0, {}});
        if (sched.bp->output_terminals.size() > src_terminal)
            command_queue.enqueue({command_type::connect, h, 0, 0, 0.0, src_terminal, dest});
        return h;
    }

    void kill(instance_handle h) { command_queue.enqueue({command_type::kill, h, 0, 0, 0.0, 0, {}}); }
    void set_parameter(instance_handle h, size_t node_index, size_t param, sample_t value) {
        command_queue.enqueue({command_type::set_parameter, h, node_index, param, value, 0, {}});
    }
    void connect(instance_handle src, size_t src_terminal, route_target dest) {
        command_queue.enqueue({command_type::connect, src, 0, 0, 0.0, src_terminal, dest});
    }
    void disconnect(instance_handle src, size_t src_terminal, route_target dest) {
        command_queue.enqueue({command_type::disconnect, src, 0, 0, 0.0, src_terminal, dest});
    }

    size_t reclaim() {
        size_t n = 0; telemetry_event ev;
        while (telemetry_queue.try_dequeue(ev)) { mgr.recycle(ev.slot); ++n; }
        return n;
    }

    bool is_valid(instance_handle h) const { return mgr.is_valid(h); }
    size_t active_count() const { return active_list.size(); }
    size_t route_count() const { return routes.size(); }

    // ---- Audio thread ----

    int process(double* out_buf, const double* /*in_buf*/, unsigned int n_frames) {
        enable_denormal_flush();          // FTZ/DAZ on the audio (callback) thread
        rt::no_alloc_scope rt_guard;      // audio path must not allocate
        const size_t nf = std::min<size_t>(n_frames, block_size);

        drain_commands();
        for (size_t ch = 0; ch < output_channels; ++ch)
            std::fill(master_buffers[ch], master_buffers[ch] + block_size, 0.0);

        // Compute every active instance's output (fills buffers + input terminals).
        if (workers && active_list.size() >= 2) compute_parallel(nf);
        else                                    compute_sequential(nf);

        // Deterministic single-threaded finish: sum to master, then reap.
        for (const auto& r : routes) {
            if (!r.to_master || !route_live(r)) continue;
            if (auto* src = mgr.instance_at(r.src_slot)) sum_terminal_to_master(src, r.src_terminal, nf);
        }
        reap_pass(nf);

        for (size_t i = 0; i < n_frames; ++i)
            for (size_t ch = 0; ch < output_channels; ++ch)
                out_buf[i * output_channels + ch] = (i < nf) ? master_buffers[ch][i] : 0.0;
        return 0;
    }

private:
    // ---- Executors ----

    void compute_sequential(size_t nf) {
        compute_order();
        for (uint32_t slot : order_scratch) {
            graph_instance* g = mgr.slots[slot].inst.get();
            gather_inputs(slot, g, nf);
            g->process();
        }
    }

    void compute_parallel(size_t nf) {
        parallel_nf = nf;

        for (uint32_t slot : active_list) dep[slot].store(0, std::memory_order_relaxed);
        for (const auto& r : routes)
            if (!r.to_master && route_live(r))
                dep[r.dst_slot].fetch_add(1, std::memory_order_relaxed);

        workers->tasks_remaining.store(static_cast<int>(active_list.size()), std::memory_order_relaxed);
        workers->block_active.store(true, std::memory_order_release);

        for (uint32_t slot : active_list)
            if (dep[slot].load(std::memory_order_relaxed) == 0)
                workers->queue.push(slot);

        // Audio thread participates as a worker until the block is drained.
        while (workers->tasks_remaining.load(std::memory_order_acquire) > 0) {
            uint32_t t;
            if (workers->queue.pop(t)) execute_instance_task(t);
            else cpu_relax();
        }
        workers->block_active.store(false, std::memory_order_release);
    }

    // A single instance-task: gather inputs, process, release downstream.
    void execute_instance_task(uint32_t slot) {
        rt::no_alloc_scope rt_guard;   // also covers worker threads
        const size_t nf = parallel_nf;
        graph_instance* g = mgr.slots[slot].inst.get();
        gather_inputs(slot, g, nf);
        g->process();
        for (const auto& r : routes) {
            if (r.to_master || r.src_slot != slot || !route_live(r)) continue;
            if (dep[r.dst_slot].fetch_sub(1, std::memory_order_acq_rel) == 1)
                workers->queue.push(r.dst_slot);
        }
        workers->tasks_remaining.fetch_sub(1, std::memory_order_acq_rel);
    }

    // Zero input terminals then sum all live incoming routes (deterministic order).
    void gather_inputs(uint32_t slot, graph_instance* g, size_t nf) {
        zero_input_terminals(g, nf);
        for (const auto& r : routes) {
            if (r.to_master || r.dst_slot != slot || !route_live(r)) continue;
            if (auto* src = mgr.instance_at(r.src_slot))
                sum_terminal_to_instance(src, r.src_terminal, g, r.dst_terminal, nf);
        }
    }

    // ---- Command handling ----

    void drain_commands() {
        command_event cmd;
        while (command_queue.try_dequeue(cmd)) {
            switch (cmd.type) {
                case command_type::activate: {
                    const uint32_t slot = cmd.handle.slot;
                    if (slot < active_gen.size() && active_gen[slot] == 0) {
                        active_gen[slot] = cmd.handle.generation;
                        active_list.push_back(slot);
                    }
                    break;
                }
                case command_type::kill: {
                    const uint32_t slot = cmd.handle.slot;
                    if (slot < active_gen.size() && active_gen[slot] == cmd.handle.generation)
                        release_slot(slot);
                    break;
                }
                case command_type::set_parameter: {
                    const uint32_t slot = cmd.handle.slot;
                    if (slot < active_gen.size() && active_gen[slot] == cmd.handle.generation)
                        if (auto* g = mgr.instance_at(slot))
                            g->set_parameter(cmd.node_index, cmd.parameter_index, cmd.parameter_value);
                    break;
                }
                case command_type::connect:    add_route(cmd);    break;
                case command_type::disconnect: remove_route(cmd); break;
            }
        }
    }

    void add_route(const command_event& cmd) {
        const instance_handle src = cmd.handle;
        if (src.slot >= active_gen.size() || active_gen[src.slot] != src.generation) return;
        if (routes.size() >= routes.capacity()) return; // never realloc on the audio thread
        active_route r;
        r.src_slot = src.slot; r.src_gen = src.generation; r.src_terminal = cmd.src_terminal;
        r.to_master = cmd.dest.to_master;
        if (!r.to_master) {
            const instance_handle d = cmd.dest.instance;
            if (d.slot >= active_gen.size() || active_gen[d.slot] != d.generation) return;
            r.dst_slot = d.slot; r.dst_gen = d.generation; r.dst_terminal = cmd.dest.terminal;
        }
        routes.push_back(r);
    }

    void remove_route(const command_event& cmd) {
        const instance_handle src = cmd.handle;
        routes.erase(std::remove_if(routes.begin(), routes.end(), [&](const active_route& r) {
            if (r.src_slot != src.slot || r.src_gen != src.generation) return false;
            if (r.src_terminal != cmd.src_terminal || r.to_master != cmd.dest.to_master) return false;
            if (!r.to_master)
                return r.dst_slot == cmd.dest.instance.slot &&
                       r.dst_gen == cmd.dest.instance.generation &&
                       r.dst_terminal == cmd.dest.terminal;
            return true;
        }), routes.end());
    }

    bool route_live(const active_route& r) const {
        if (r.src_slot >= active_gen.size() || active_gen[r.src_slot] != r.src_gen) return false;
        if (r.to_master) return true;
        return r.dst_slot < active_gen.size() && active_gen[r.dst_slot] == r.dst_gen;
    }

    void compute_order() {
        for (uint32_t slot : active_list) indeg_scratch[slot] = 0;
        for (const auto& r : routes)
            if (!r.to_master && route_live(r)) indeg_scratch[r.dst_slot]++;

        order_scratch.clear();
        for (uint32_t slot : active_list)
            if (indeg_scratch[slot] == 0) order_scratch.push_back(slot);

        for (size_t head = 0; head < order_scratch.size(); ++head) {
            const uint32_t u = order_scratch[head];
            for (const auto& r : routes) {
                if (r.to_master || !route_live(r) || r.src_slot != u) continue;
                if (--indeg_scratch[r.dst_slot] == 0) order_scratch.push_back(r.dst_slot);
            }
        }
        if (order_scratch.size() < active_list.size())
            for (uint32_t slot : active_list)
                if (indeg_scratch[slot] > 0) order_scratch.push_back(slot);
    }

    void reap_pass(size_t nf) {
        reap_scratch.clear();
        for (uint32_t slot : active_list) {
            instance_slot& s = mgr.slots[slot];
            if (should_reap(s, s.inst.get(), terminal_peak(s.inst.get(), nf)))
                reap_scratch.push_back(slot);
        }
        for (uint32_t slot : reap_scratch) release_slot(slot);
    }

    // ---- Buffer helpers ----

    void zero_input_terminals(graph_instance* g, size_t nf) {
        for (size_t t = 0; t < g->input_terminal_count(); ++t)
            for (size_t c = 0; c < g->input_terminal_channels(t); ++c) {
                audio_buffer_view v = g->input_terminal_view(t, c);
                for (size_t i = 0; i < nf; ++i) v[i] = 0.0;
            }
    }

    static void sum_terminal_to_instance(graph_instance* src, size_t src_t,
                                         graph_instance* dst, size_t dst_t, size_t nf) {
        const size_t ch = std::min(src->output_terminal_channels(src_t),
                                   dst->input_terminal_channels(dst_t));
        for (size_t c = 0; c < ch; ++c) {
            audio_buffer_view sv = src->output_terminal_view(src_t, c);
            audio_buffer_view dv = dst->input_terminal_view(dst_t, c);
            for (size_t i = 0; i < nf; ++i) dv[i] += sv[i];
        }
    }

    void sum_terminal_to_master(graph_instance* src, size_t src_t, size_t nf) {
        const size_t ch = std::min(src->output_terminal_channels(src_t), output_channels);
        for (size_t c = 0; c < ch; ++c) {
            audio_buffer_view sv = src->output_terminal_view(src_t, c);
            for (size_t i = 0; i < nf; ++i) master_buffers[c][i] += sv[i];
        }
    }

    static sample_t terminal_peak(graph_instance* g, size_t nf) {
        if (g->output_terminal_count() == 0) return 0.0;
        sample_t p = 0.0;
        const size_t ch = g->output_terminal_channels(0);
        for (size_t c = 0; c < ch; ++c) {
            audio_buffer_view v = g->output_terminal_view(0, c);
            for (size_t i = 0; i < nf; ++i) p = std::max(p, std::fabs(v[i]));
        }
        return p;
    }

    void release_slot(uint32_t slot) {
        active_gen[slot] = 0;
        auto it = std::find(active_list.begin(), active_list.end(), slot);
        if (it != active_list.end()) active_list.erase(it);
        routes.erase(std::remove_if(routes.begin(), routes.end(), [slot](const active_route& r) {
            return r.src_slot == slot || (!r.to_master && r.dst_slot == slot);
        }), routes.end());
        // Produced on the audio thread: try_enqueue never grows the queue.
        telemetry_queue.try_enqueue({slot});
    }

    static bool should_reap(instance_slot& s, graph_instance* g, sample_t block_peak) {
        if (g->finished_flag) return true;
        s.age_blocks++;
        switch (s.life.kind) {
            case lifetime_kind::permanent:      return false;
            case lifetime_kind::timed:          return s.age_blocks >= s.life.ttl_blocks;
            case lifetime_kind::until_finished: return false;
            case lifetime_kind::until_silent:
                if (block_peak < s.life.silence_threshold) s.silent_run++;
                else s.silent_run = 0;
                return s.silent_run >= s.life.silence_blocks;
        }
        return false;
    }
};

} // namespace xrune
