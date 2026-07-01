#pragma once
#include "core.hpp"
#include "blueprint.hpp"
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

inline bool is_power_of_two(size_t x) { return x && (x & (x - 1)) == 0; }

inline compiled_schedule compile(const graph_blueprint& bp, size_t block_size) {
    compiled_schedule s;
    s.bp = &bp;
    s.block_size = block_size;
    const size_t n = bp.size();

    // --- Kahn topological sort (audio + param connections are dependencies) ---
    std::vector<size_t> in_degree(n, 0);
    std::vector<std::vector<size_t>> adj(n);
    auto add_edge = [&](size_t u, size_t v) { adj[u].push_back(v); in_degree[v]++; };
    for (const auto& c : bp.connections)       add_edge(c.src_node, c.dst_node);
    for (const auto& c : bp.param_connections) add_edge(c.src_node, c.dst_node);

    std::queue<size_t> q;
    for (size_t i = 0; i < n; ++i) if (in_degree[i] == 0) q.push(i);
    while (!q.empty()) {
        size_t u = q.front(); q.pop();
        s.topo_order.push_back(u);
        for (size_t v : adj[u]) if (--in_degree[v] == 0) q.push(v);
    }
    if (s.topo_order.size() != n) { s.ok = false; return s; }

    // --- Rate + block-size propagation & validation ---
    // region_rate is the sample-rate multiplier (power of two). blk_exp is the
    // block-size exponent (block = B * 2^blk_exp). Both propagate along edges and
    // must be consistent across a node's inputs. calls-per-cycle then follows
    // from throughput (region_rate*B) divided by the per-call block.
    s.region_rate.assign(n, 1);
    s.node_block.assign(n, block_size);
    s.calls.assign(n, 1);
    std::vector<long> blk_exp(n, 0);

    for (size_t v : s.topo_order) {
        long in_rate = -1, in_blk = 0;
        bool consistent = true;
        auto consider = [&](size_t src) {
            long r = static_cast<long>(s.region_rate[src]);
            long b = blk_exp[src];
            if (in_rate == -1) { in_rate = r; in_blk = b; }
            else if (r != in_rate || b != in_blk) consistent = false;
        };
        for (const auto& c : bp.connections)       if (c.dst_node == v) consider(c.src_node);
        for (const auto& c : bp.param_connections) if (c.dst_node == v) consider(c.src_node);
        if (!consistent) { s.ok = false; return s; }

        const size_t base_rate = (in_rate < 0) ? 1 : static_cast<size_t>(in_rate);
        const long   base_blk  = (in_rate < 0) ? 0 : in_blk;

        // Sample-rate axis.
        const size_t rnum = bp.nodes[v]->rate_num(), rden = bp.nodes[v]->rate_den();
        if (rden == 0 || (base_rate * rnum) % rden != 0) { s.ok = false; return s; }
        const size_t R = (base_rate * rnum) / rden;
        if (!is_power_of_two(R)) { s.ok = false; return s; }

        // Block-size axis (factor of two only).
        const size_t bnum = bp.nodes[v]->block_num(), bden = bp.nodes[v]->block_den();
        long delta;
        if (bnum == 1 && bden == 1) delta = 0;
        else if (bnum == 2 && bden == 1) delta = 1;
        else if (bnum == 1 && bden == 2) delta = -1;
        else { s.ok = false; return s; }
        const long be = base_blk + delta;

        // Per-call block = B * 2^be.
        size_t nb;
        if (be >= 0) nb = block_size << be;
        else { const size_t d = size_t(1) << (-be); if (block_size % d != 0) { s.ok = false; return s; } nb = block_size / d; }
        if (nb == 0) { s.ok = false; return s; }

        // calls = throughput / per-call block. Must be a positive integer;
        // C < 1 would mean the node spans multiple cycles (cross-cycle
        // block-increase), which is not yet supported.
        const size_t T = R * block_size;
        if (T % nb != 0) { s.ok = false; return s; }
        const size_t C = T / nb;
        if (C == 0) { s.ok = false; return s; }

        s.region_rate[v] = R;
        blk_exp[v] = be;
        s.node_block[v] = nb;
        s.calls[v] = C;
    }

    // --- per-(node,call) context indexing ---
    s.ctx_base.resize(n);
    size_t ci = 0;
    for (size_t i = 0; i < n; ++i) { s.ctx_base[i] = ci; ci += s.calls[i]; }
    s.total_calls = ci;

    // --- Output buffer layout (sample offsets) ---
    s.out_base_sample.resize(n);
    s.out_stride.resize(n);
    size_t off = 0;
    for (size_t i = 0; i < n; ++i) {
        const size_t stride = s.region_rate[i] * block_size;
        s.out_stride[i] = stride;
        s.out_base_sample[i] = off;
        off += bp.nodes[i]->outputs_count() * stride;
        s.max_stride = std::max(s.max_stride, stride);
    }
    s.total_output_samples = off;
    if (s.max_stride == 0) s.max_stride = block_size;

    // --- State layout ---
    s.state_offset.resize(n);
    s.state_bytes.resize(n);
    size_t soff = 0;
    for (size_t i = 0; i < n; ++i) {
        const size_t sz = bp.nodes[i]->state_size();
        const size_t al = bp.nodes[i]->state_align();
        if (sz > 0) { soff = (soff + (al - 1)) & ~(al - 1); s.state_offset[i] = soff; soff += sz; }
        else s.state_offset[i] = 0;
        s.state_bytes[i] = sz;
    }
    s.total_state_bytes = soff;

    // --- Input / param wiring ---
    s.input_source.resize(n);
    s.param_source.resize(n);
    s.param_base.resize(n);
    size_t p = 0;
    for (size_t i = 0; i < n; ++i) {
        s.input_source[i].assign(bp.nodes[i]->inputs_count(), src_ref{});
        s.param_source[i].assign(bp.nodes[i]->params_count(), src_ref{});
        s.param_base[i] = p;
        p += bp.nodes[i]->params_count();
    }
    s.total_params = p;
    for (const auto& c : bp.connections)
        s.input_source[c.dst_node][c.dst_input] = src_ref{static_cast<long>(c.src_node), c.src_output};
    for (const auto& c : bp.param_connections)
        s.param_source[c.dst_node][c.dst_param] = src_ref{static_cast<long>(c.src_node), c.src_output};

    s.ok = true;
    return s;
}

} // namespace xrune
