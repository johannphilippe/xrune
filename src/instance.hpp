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
    sample_t current = 0.0;
    sample_t target = 0.0;
};

// A live realization of a blueprint. Owns an arena holding this voice's
// node-state blocks, output buffers, control state and the precomputed
// per-(node,call) processing contexts. Multi-rate (Phase 5): a node is executed
// `calls == region_rate` times per cycle, each call operating on a sliced view
// of the producer/consumer buffers; single-rate graphs collapse to one call.
struct graph_instance {
    const compiled_schedule* sched = nullptr;
    memory_arena arena;

    std::byte* state_region = nullptr;
    sample_t* buffer_pool = nullptr;       // total_output_samples
    sample_t* silent = nullptr;            // read-only zeros, >= max slice size
    audio_buffer_view* views = nullptr;    // flattened per-(node,call) in/out views
    node_processing_context* contexts = nullptr; // one per (node,call)
    port_control* controls = nullptr;      // per control port
    param_view* param_views = nullptr;     // flattened per-(node,call) control ports

    size_t block_size = 0;                 // base block B
    size_t sample_rate = 48000;            // base sample rate
    bool finished_flag = false;

    graph_instance() = default;
    ~graph_instance() { teardown(); }
    graph_instance(const graph_instance&) = delete;
    graph_instance& operator=(const graph_instance&) = delete;

    // Release per-node hosted state (Faust/Csound dsp instances, etc.).
    void teardown() {
        if (!sched || !state_region) return;
        const size_t n = sched->bp->size();
        for (size_t i = 0; i < n; ++i)
            if (sched->state_bytes[i])
                sched->bp->nodes[i]->destroy_state(state_region + sched->state_offset[i]);
        sched = nullptr; // idempotent
    }

    size_t out_offset(size_t node, size_t ch) const {
        return sched->out_base_sample[node] + ch * sched->out_stride[node];
    }

    bool build(const compiled_schedule& s, size_t sr) {
        if (!s.ok) return false;
        sched = &s;
        block_size = s.block_size;
        sample_rate = sr;
        const size_t n = s.bp->size();
        const size_t B = block_size;

        size_t total_views = 0, total_pv = 0;
        for (size_t i = 0; i < n; ++i) {
            const size_t io = s.bp->nodes[i]->inputs_count() + s.bp->nodes[i]->outputs_count();
            total_views += s.calls[i] * io;
            total_pv += s.calls[i] * s.bp->nodes[i]->params_count();
        }
        const size_t silent_size = std::max(s.max_stride, 2 * B);

        size_t bytes = 0;
        bytes += s.total_state_bytes + 64;
        bytes += s.total_output_samples * sizeof(sample_t) + 64;
        bytes += silent_size * sizeof(sample_t) + 64;
        bytes += total_views * sizeof(audio_buffer_view) + 64;
        bytes += s.total_calls * sizeof(node_processing_context) + 64;
        bytes += s.total_params * sizeof(port_control) + 64;
        bytes += total_pv * sizeof(param_view) + 64;
        bytes += 256;
        arena.reserve(bytes);

        state_region = s.total_state_bytes
            ? static_cast<std::byte*>(arena.allocate(s.total_state_bytes, alignof(std::max_align_t)))
            : nullptr;
        buffer_pool = s.total_output_samples ? arena.allocate_array<sample_t>(s.total_output_samples) : nullptr;
        silent = arena.allocate_array<sample_t>(silent_size);
        views = total_views ? arena.allocate_array<audio_buffer_view>(total_views) : nullptr;
        contexts = arena.allocate_array<node_processing_context>(s.total_calls);
        controls = s.total_params ? arena.allocate_array<port_control>(s.total_params) : nullptr;
        param_views = total_pv ? arena.allocate_array<param_view>(total_pv) : nullptr;

        if ((s.total_state_bytes && !state_region) || !silent || !contexts ||
            (s.total_output_samples && !buffer_pool) || (total_views && !views) ||
            (s.total_params && !controls) || (total_pv && !param_views))
            return false;

        for (size_t i = 0; i < s.total_output_samples; ++i) buffer_pool[i] = 0.0;
        for (size_t i = 0; i < silent_size; ++i) silent[i] = 0.0;

        for (size_t i = 0; i < n; ++i)
            if (s.state_bytes[i]) {
                node* nd = s.bp->nodes[i].get();
                void* st = state_region + s.state_offset[i];
                nd->init_state(st);
                nd->setup_state(st, sample_rate, block_size);
            }

        // Precompute per-(node,call) contexts and buffer/param views.
        size_t vi = 0, pvi = 0;
        for (size_t i = 0; i < n; ++i) {
            node* nd = s.bp->nodes[i].get();
            const size_t ic = nd->inputs_count();
            const size_t oc = nd->outputs_count();
            const size_t pc = nd->params_count();
            const size_t C = s.calls[i];
            const size_t nb = s.node_block[i]; // per-call block size (block-size axis)
            const size_t pbase = s.param_base[i];
            const size_t silent_in = (nd->rate_num() ? nb * nd->rate_den() / nd->rate_num() : nb);

            for (size_t pi = 0; pi < pc; ++pi) {
                const sample_t def = nd->param_default(pi);
                controls[pbase + pi] = port_control{def, def};
            }

            for (size_t k = 0; k < C; ++k) {
                audio_buffer_view* in_views = views + vi;  vi += ic;
                audio_buffer_view* out_views = views + vi; vi += oc;
                param_view* pv = param_views + pvi;        pvi += pc;

                for (size_t c = 0; c < oc; ++c)
                    out_views[c] = audio_buffer_view(buffer_pool + out_offset(i, c) + k * nb, nb);

                for (size_t c = 0; c < ic; ++c) {
                    const src_ref sr = s.input_source[i][c];
                    if (sr.silent()) {
                        in_views[c] = audio_buffer_view(silent, silent_in);
                    } else {
                        const size_t per_call = s.out_stride[sr.node] / C;
                        in_views[c] = audio_buffer_view(
                            buffer_pool + out_offset(sr.node, sr.chan) + k * per_call, per_call);
                    }
                }

                for (size_t pi = 0; pi < pc; ++pi) {
                    const src_ref sr = s.param_source[i][pi];
                    if (sr.silent()) {
                        pv[pi] = param_view{}; // control-rate; base/inc set at runtime
                        pv[pi].base = nd->param_default(pi);
                    } else {
                        const size_t per_call = s.out_stride[sr.node] / C;
                        pv[pi].buffer = buffer_pool + out_offset(sr.node, sr.chan) + k * per_call;
                    }
                }

                auto* ctx = new (&contexts[s.ctx_base[i] + k]) node_processing_context();
                ctx->inputs = in_views;
                ctx->input_count = ic;
                ctx->outputs = out_views;
                ctx->output_count = oc;
                ctx->params = pv;
                ctx->param_count = pc;
                ctx->sample_rate = sample_rate * s.region_rate[i];
                ctx->block_size = nb;
            }
        }
        return true;
    }

    void* state_ptr(size_t node_index) const {
        return sched->state_bytes[node_index]
            ? static_cast<void*>(state_region + sched->state_offset[node_index])
            : nullptr;
    }

    // Execute one cycle: each node in topological order, called region_rate times.
    void process() {
        for (size_t idx : sched->topo_order) {
            const size_t C = sched->calls[idx];
            for (size_t k = 0; k < C; ++k) {
                node_processing_context& ctx = contexts[sched->ctx_base[idx] + k];
                update_control_ramps(idx, ctx);
                sched->bp->nodes[idx]->process(state_ptr(idx), ctx);
            }
        }
    }

    void update_control_ramps(size_t node_index, node_processing_context& ctx) {
        const size_t pc = ctx.param_count;
        const size_t pbase = sched->param_base[node_index];
        const sample_t inv_bs = ctx.block_size ? (1.0 / static_cast<sample_t>(ctx.block_size)) : 0.0;
        auto* pv = const_cast<param_view*>(ctx.params);
        for (size_t pi = 0; pi < pc; ++pi) {
            if (pv[pi].buffer) continue; // audio-rate
            port_control& c = controls[pbase + pi];
            pv[pi].base = c.current;
            pv[pi].inc = (c.target - c.current) * inv_bs;
            c.current = c.target;
        }
    }

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
        return node_output_view(static_cast<size_t>(sched->bp->output_node), ch);
    }

    // Full per-channel output buffer of a node (size = region_rate * block_size).
    audio_buffer_view node_output_view(size_t node_index, size_t ch) const {
        return audio_buffer_view(buffer_pool + out_offset(node_index, ch), sched->out_stride[node_index]);
    }

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

inline std::unique_ptr<graph_instance> instantiate(const compiled_schedule& s, size_t sample_rate) {
    auto inst = std::make_unique<graph_instance>();
    if (!inst->build(s, sample_rate)) return nullptr;
    return inst;
}

} // namespace xrune
