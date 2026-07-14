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

#include "xrune/engine.hpp"

#include <algorithm>

namespace xrune {

engine::~engine() { stop(); }

void engine::use_backend(std::unique_ptr<audio_backend> b) { backend = std::move(b); }

bool engine::init(size_t sr , size_t bs , size_t ins , size_t outs ,
              size_t max_instances , size_t num_workers ) {
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

bool engine::start() {
    if (num_worker_threads > 0 && !workers) {
        workers = std::make_unique<worker_pool>(next_pow2(std::max<size_t>(mgr.capacity(), 2)));
        workers->start(num_worker_threads, [this](uint32_t slot) { execute_instance_task(slot); });
    }
    return backend ? backend->start() : false;
}

void engine::stop() {
    if (workers) { workers->stop(); workers.reset(); }
    if (backend) { backend->stop(); backend->close(); }
}

instance_handle engine::spawn(const compiled_schedule& sched, lifetime_policy life ,
                          route_target dest , size_t src_terminal ,
                          const std::vector<initial_param>* init ) {
    reclaim();
    instance_handle h = mgr.create(sched, sample_rate, life);
    if (!h.valid()) return null_handle;

    // The instance exists but is not active yet, and we are on the control
    // thread -- so we can write its ports directly. init_parameter() sets
    // current AND target, so there is no glide from the compiled default.
    if (init && !init->empty()) {
        if (graph_instance* g = mgr.instance_at(h.slot))
            for (const initial_param& ip : *init)
                g->init_parameter(ip.node, ip.param, ip.value);
    }

    command_queue.enqueue({command_type::activate, h, 0, 0, 0.0, 0, {}});
    if (sched.bp->output_terminals.size() > src_terminal)
        command_queue.enqueue({command_type::connect, h, 0, 0, 0.0, src_terminal, dest});
    return h;
}

void engine::kill(instance_handle h) { command_queue.enqueue({command_type::kill, h, 0, 0, 0.0, 0, {}}); }

void engine::set_parameter(instance_handle h, size_t node_index, size_t param, sample_t value) {
    command_queue.enqueue({command_type::set_parameter, h, node_index, param, value, 0, {}});
}

void engine::connect(instance_handle src, size_t src_terminal, route_target dest) {
    command_queue.enqueue({command_type::connect, src, 0, 0, 0.0, src_terminal, dest});
}

void engine::disconnect(instance_handle src, size_t src_terminal, route_target dest) {
    command_queue.enqueue({command_type::disconnect, src, 0, 0, 0.0, src_terminal, dest});
}

size_t engine::reclaim(std::vector<voice_end>* out) {
    size_t n = 0;
    telemetry_event ev;
    while (telemetry_queue.try_dequeue(ev)) {
        // Report BEFORE recycling: recycle() bumps the slot's generation, and the
        // host needs the handle it actually holds.
        if (out) out->push_back({instance_handle{ev.slot, ev.generation}, ev.reason});
        mgr.recycle(ev.slot);
        ++n;
    }
    return n;
}

bool engine::is_valid(instance_handle h) const { return mgr.is_valid(h); }

size_t engine::active_count() const { return active_list.size(); }

size_t engine::route_count() const { return routes.size(); }

int engine::process(double* out_buf, const double* /*in_buf*/, unsigned int n_frames) {
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

void engine::compute_sequential(size_t nf) {
    compute_order();
    for (uint32_t slot : order_scratch) {
        graph_instance* g = mgr.slots[slot].inst.get();
        gather_inputs(slot, g, nf);
        g->process();
    }
}

void engine::compute_parallel(size_t nf) {
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

void engine::execute_instance_task(uint32_t slot) {
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

void engine::gather_inputs(uint32_t slot, graph_instance* g, size_t nf) {
    zero_input_terminals(g, nf);
    for (const auto& r : routes) {
        if (r.to_master || r.dst_slot != slot || !route_live(r)) continue;
        if (auto* src = mgr.instance_at(r.src_slot))
            sum_terminal_to_instance(src, r.src_terminal, g, r.dst_terminal, nf);
    }
}

void engine::drain_commands() {
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
                    release_slot(slot, end_reason::killed);
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

void engine::add_route(const command_event& cmd) {
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

void engine::remove_route(const command_event& cmd) {
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

bool engine::route_live(const active_route& r) const {
    if (r.src_slot >= active_gen.size() || active_gen[r.src_slot] != r.src_gen) return false;
    if (r.to_master) return true;
    return r.dst_slot < active_gen.size() && active_gen[r.dst_slot] == r.dst_gen;
}

void engine::compute_order() {
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

void engine::reap_pass(size_t nf) {
    reap_scratch.clear();
    reap_reasons.clear();
    for (uint32_t slot : active_list) {
        instance_slot& s = mgr.slots[slot];
        end_reason why = end_reason::finished;
        if (should_reap(s, s.inst.get(), terminal_peak(s.inst.get(), nf), why)) {
            reap_scratch.push_back(slot);
            reap_reasons.push_back(why);
        }
    }
    for (size_t i = 0; i < reap_scratch.size(); ++i)
        release_slot(reap_scratch[i], reap_reasons[i]);
}

void engine::zero_input_terminals(graph_instance* g, size_t nf) {
    for (size_t t = 0; t < g->input_terminal_count(); ++t)
        for (size_t c = 0; c < g->input_terminal_channels(t); ++c) {
            audio_buffer_view v = g->input_terminal_view(t, c);
            for (size_t i = 0; i < nf; ++i) v[i] = 0.0;
        }
}

void engine::sum_terminal_to_instance(graph_instance* src, size_t src_t,
                                         graph_instance* dst, size_t dst_t, size_t nf) {
    const size_t ch = std::min(src->output_terminal_channels(src_t),
                               dst->input_terminal_channels(dst_t));
    for (size_t c = 0; c < ch; ++c) {
        audio_buffer_view sv = src->output_terminal_view(src_t, c);
        audio_buffer_view dv = dst->input_terminal_view(dst_t, c);
        for (size_t i = 0; i < nf; ++i) dv[i] += sv[i];
    }
}

void engine::sum_terminal_to_master(graph_instance* src, size_t src_t, size_t nf) {
    const size_t ch = std::min(src->output_terminal_channels(src_t), output_channels);
    for (size_t c = 0; c < ch; ++c) {
        audio_buffer_view sv = src->output_terminal_view(src_t, c);
        for (size_t i = 0; i < nf; ++i) master_buffers[c][i] += sv[i];
    }
}

sample_t engine::terminal_peak(graph_instance* g, size_t nf) {
    if (g->output_terminal_count() == 0) return 0.0;
    sample_t p = 0.0;
    const size_t ch = g->output_terminal_channels(0);
    for (size_t c = 0; c < ch; ++c) {
        audio_buffer_view v = g->output_terminal_view(0, c);
        for (size_t i = 0; i < nf; ++i) p = std::max(p, std::fabs(v[i]));
    }
    return p;
}

void engine::release_slot(uint32_t slot, end_reason why) {
    const uint32_t gen = active_gen[slot];
    active_gen[slot] = 0;
    auto it = std::find(active_list.begin(), active_list.end(), slot);
    if (it != active_list.end()) active_list.erase(it);
    routes.erase(std::remove_if(routes.begin(), routes.end(), [slot](const active_route& r) {
        return r.src_slot == slot || (!r.to_master && r.dst_slot == slot);
    }), routes.end());
    // Produced on the audio thread: try_enqueue never grows the queue.
    telemetry_queue.try_enqueue({slot, gen, why});
}

bool engine::should_reap(instance_slot& s, graph_instance* g, sample_t block_peak,
                         end_reason& why) {
    if (g->finished_flag) { why = end_reason::finished; return true; }
    s.age_blocks++;
    switch (s.life.kind) {
        case lifetime_kind::permanent:      return false;
        case lifetime_kind::timed:
            if (s.age_blocks >= s.life.ttl_blocks) { why = end_reason::timed_out; return true; }
            return false;
        case lifetime_kind::until_finished: return false;
        case lifetime_kind::until_silent:
            if (block_peak < s.life.silence_threshold) s.silent_run++;
            else s.silent_run = 0;
            if (s.silent_run >= s.life.silence_blocks) { why = end_reason::silent; return true; }
            return false;
    }
    return false;
}

} // namespace xrune
