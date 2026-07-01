#pragma once
#include "core.hpp"
#include "blueprint.hpp"
#include "schedule.hpp"
#include "arena.hpp"
#include <memory>
#include <new>
#include <algorithm>

namespace xrune {

// Per-instance, per-control-port smoothing state (control-rate ports only).
struct port_control {
    sample_t current = 0.0; // value at the start of the current block
    sample_t target = 0.0;  // value being ramped toward
};

// A live realization of a blueprint (pre_roadmap §2). Owns an arena holding this
// voice's node-state blocks, output buffers, and the precomputed processing
// contexts. Many instances share one compiled_schedule. Phase 1 executor is
// single-threaded; the parallel executor arrives in Phase 6.
struct graph_instance {
    const compiled_schedule* sched = nullptr;
    memory_arena arena;

    std::byte* state_region = nullptr;         // all node state blocks, packed
    sample_t* buffer_pool = nullptr;           // total_output_slots * block_size
    sample_t* silent = nullptr;                // block_size of zeros (read-only)
    audio_buffer_view* views = nullptr;        // flattened per-node in/out views
    node_processing_context* contexts = nullptr;
    port_control* controls = nullptr;          // per control port (total_params)
    param_view* param_views = nullptr;         // flattened per-node control ports

    size_t block_size = 0;
    size_t sample_rate = 48000;
    bool finished_flag = false;

    // Allocate + initialize everything for one voice. Control-thread only.
    bool build(const compiled_schedule& s, size_t sr) {
        if (!s.ok) return false;
        sched = &s;
        block_size = s.block_size;
        sample_rate = sr;
        const size_t n = s.bp->size();

        size_t total_views = 0;
        for (size_t i = 0; i < n; ++i)
            total_views += s.bp->nodes[i]->inputs_count() + s.bp->nodes[i]->outputs_count();

        // Size the arena (generous per-allocation alignment slack).
        size_t bytes = 0;
        bytes += s.total_state_bytes + 64;
        bytes += (s.total_output_slots * block_size) * sizeof(sample_t) + 64;
        bytes += block_size * sizeof(sample_t) + 64;
        bytes += total_views * sizeof(audio_buffer_view) + 64;
        bytes += n * sizeof(node_processing_context) + 64;
        bytes += s.total_params * sizeof(port_control) + 64;
        bytes += s.total_params * sizeof(param_view) + 64;
        bytes += 256;
        arena.reserve(bytes);

        state_region = s.total_state_bytes
            ? static_cast<std::byte*>(arena.allocate(s.total_state_bytes, alignof(std::max_align_t)))
            : nullptr;
        buffer_pool = arena.allocate_array<sample_t>(s.total_output_slots * block_size);
        silent = arena.allocate_array<sample_t>(block_size);
        views = arena.allocate_array<audio_buffer_view>(total_views);
        contexts = arena.allocate_array<node_processing_context>(n);
        controls = s.total_params ? arena.allocate_array<port_control>(s.total_params) : nullptr;
        param_views = s.total_params ? arena.allocate_array<param_view>(s.total_params) : nullptr;

        if ((s.total_state_bytes && !state_region) || !silent || !contexts ||
            (s.total_output_slots && !buffer_pool) || (total_views && !views) ||
            (s.total_params && (!controls || !param_views)))
            return false;

        for (size_t i = 0; i < s.total_output_slots * block_size; ++i) buffer_pool[i] = 0.0;
        for (size_t i = 0; i < block_size; ++i) silent[i] = 0.0;

        for (size_t i = 0; i < n; ++i)
            if (s.state_bytes[i])
                s.bp->nodes[i]->init_state(state_region + s.state_offset[i]);

        // Precompute buffer views + processing contexts once.
        size_t voff = 0;
        for (size_t i = 0; i < n; ++i) {
            node* nd = s.bp->nodes[i].get();
            const size_t ic = nd->inputs_count();
            const size_t oc = nd->outputs_count();

            audio_buffer_view* in_views = views + voff;  voff += ic;
            audio_buffer_view* out_views = views + voff;  voff += oc;

            for (size_t ch = 0; ch < oc; ++ch)
                out_views[ch] = audio_buffer_view(
                    buffer_pool + (s.output_slot_base[i] + ch) * block_size, block_size);

            for (size_t ch = 0; ch < ic; ++ch) {
                const long src = s.input_source[i][ch];
                sample_t* ptr = (src == SILENT_SLOT)
                    ? silent
                    : buffer_pool + static_cast<size_t>(src) * block_size;
                in_views[ch] = audio_buffer_view(ptr, block_size);
            }

            // Control ports: initialize control state to defaults and bind each
            // port to its source buffer (audio-rate) or leave it control-rate.
            const size_t pc = nd->params_count();
            const size_t pbase = s.param_base[i];
            for (size_t pi = 0; pi < pc; ++pi) {
                const sample_t def = nd->param_default(pi);
                controls[pbase + pi] = port_control{def, def};
                param_view& pv = param_views[pbase + pi];
                const long src = s.param_source[i][pi];
                if (src == SILENT_SLOT) {
                    pv.buffer = nullptr;
                    pv.base = def;
                    pv.inc = 0.0;
                } else {
                    pv.buffer = buffer_pool + static_cast<size_t>(src) * block_size;
                }
            }

            auto* ctx = new (&contexts[i]) node_processing_context();
            ctx->inputs = in_views;
            ctx->input_count = ic;
            ctx->outputs = out_views;
            ctx->output_count = oc;
            ctx->params = param_views + pbase;
            ctx->param_count = pc;
            ctx->sample_rate = sample_rate;
            ctx->block_size = block_size;
        }
        return true;
    }

    void* state_ptr(size_t node_index) const {
        return sched->state_bytes[node_index]
            ? static_cast<void*>(state_region + sched->state_offset[node_index])
            : nullptr;
    }

    // Single-threaded block execution in topological order.
    void process() {
        for (size_t idx : sched->topo_order) {
            update_control_ramps(idx);
            sched->bp->nodes[idx]->process(state_ptr(idx), contexts[idx]);
        }
    }

    // Refresh a node's control-rate ports: build a click-free linear ramp from
    // last block's value to the target, then advance. Audio-rate ports (bound to
    // a source buffer) are untouched.
    void update_control_ramps(size_t node_index) {
        const size_t pc = sched->bp->nodes[node_index]->params_count();
        const size_t pbase = sched->param_base[node_index];
        const sample_t inv_bs = block_size ? (1.0 / static_cast<sample_t>(block_size)) : 0.0;
        for (size_t pi = 0; pi < pc; ++pi) {
            param_view& pv = param_views[pbase + pi];
            if (pv.buffer) continue; // audio-rate
            port_control& c = controls[pbase + pi];
            pv.base = c.current;
            pv.inc = (c.target - c.current) * inv_bs;
            c.current = c.target;
        }
    }

    // Set a control port's target (control thread / command). Clamped to range;
    // the value ramps in over the next block. No-op for audio-rate-driven ports.
    void set_parameter(size_t node_index, size_t param, sample_t value) {
        const node* nd = sched->bp->nodes[node_index].get();
        if (param >= nd->params_count()) return;
        const port_descriptor* pd = nd->params();
        if (pd) value = std::clamp(value, pd[param].min_value, pd[param].max_value);
        controls[sched->param_base[node_index] + param].target = value;
    }

    size_t output_channels() const {
        if (sched->bp->output_node < 0) return 0;
        return sched->bp->nodes[static_cast<size_t>(sched->bp->output_node)]->outputs_count();
    }

    audio_buffer_view output_view(size_t ch) const {
        const size_t idx = static_cast<size_t>(sched->bp->output_node);
        return node_output_view(idx, ch);
    }

    // A node's output buffer for a given channel (also used as the writable
    // entry buffer of a bus_input node for input terminals).
    audio_buffer_view node_output_view(size_t node_index, size_t ch) const {
        return audio_buffer_view(
            buffer_pool + (sched->output_slot_base[node_index] + ch) * block_size, block_size);
    }

    // ---- Terminal access (cross-instance routing, Phase 3) ----
    size_t output_terminal_count() const { return sched->bp->output_terminals.size(); }
    size_t input_terminal_count() const { return sched->bp->input_terminals.size(); }

    size_t output_terminal_channels(size_t t) const {
        return sched->bp->nodes[sched->bp->output_terminals[t].node]->outputs_count();
    }
    size_t input_terminal_channels(size_t t) const {
        return sched->bp->nodes[sched->bp->input_terminals[t].node]->outputs_count();
    }
    audio_buffer_view output_terminal_view(size_t t, size_t ch) const {
        return node_output_view(sched->bp->output_terminals[t].node, ch);
    }
    audio_buffer_view input_terminal_view(size_t t, size_t ch) const {
        return node_output_view(sched->bp->input_terminals[t].node, ch);
    }
};

// Convenience factory: build a voice or return nullptr on failure.
inline std::unique_ptr<graph_instance> instantiate(const compiled_schedule& s, size_t sample_rate) {
    auto inst = std::make_unique<graph_instance>();
    if (!inst->build(s, sample_rate)) return nullptr;
    return inst;
}

} // namespace xrune
