#pragma once
#include "core.hpp"
#include "blueprint.hpp"
#include <vector>
#include <queue>

namespace xrune {

// Sentinel: an input channel with no incoming connection reads a silent buffer.
constexpr long SILENT_SLOT = -1;

// Result of compiling a blueprint (control thread). Immutable; shared by all
// instances of that blueprint. Holds the execution order plus the per-instance
// memory layout (output buffer slots, state-block offsets, input wiring), all
// computed once so instantiation and processing allocate/branch minimally.
struct compiled_schedule {
    const graph_blueprint* bp = nullptr;
    size_t block_size = 0;
    bool ok = false;

    std::vector<size_t> topo_order;       // node indices in execution order
    std::vector<size_t> output_slot_base; // per node: index of its first output buffer slot
    size_t total_output_slots = 0;

    std::vector<size_t> state_offset;     // per node: byte offset into the state region
    std::vector<size_t> state_bytes;      // per node: state size (0 = stateless)
    size_t total_state_bytes = 0;

    // per node -> per input channel: source output slot, or SILENT_SLOT.
    std::vector<std::vector<long>> input_source;

    // Control ports (Phase 4).
    std::vector<size_t> param_base;       // per node: first index into the flat param arrays
    size_t total_params = 0;
    // per node -> per control port: source output slot (audio-rate) or SILENT_SLOT (control-rate).
    std::vector<std::vector<long>> param_source;
};

inline compiled_schedule compile(const graph_blueprint& bp, size_t block_size) {
    compiled_schedule s;
    s.bp = &bp;
    s.block_size = block_size;
    const size_t n = bp.size();

    // --- Kahn topological sort with cycle detection ---
    // Both audio connections and param (modulation) connections create
    // producer->consumer dependencies.
    std::vector<size_t> in_degree(n, 0);
    std::vector<std::vector<size_t>> adj(n);
    for (const auto& c : bp.connections) {
        adj[c.src_node].push_back(c.dst_node);
        in_degree[c.dst_node]++;
    }
    for (const auto& c : bp.param_connections) {
        adj[c.src_node].push_back(c.dst_node);
        in_degree[c.dst_node]++;
    }
    std::queue<size_t> q;
    for (size_t i = 0; i < n; ++i)
        if (in_degree[i] == 0) q.push(i);
    while (!q.empty()) {
        size_t u = q.front(); q.pop();
        s.topo_order.push_back(u);
        for (size_t v : adj[u])
            if (--in_degree[v] == 0) q.push(v);
    }
    if (s.topo_order.size() != n) { s.ok = false; return s; } // cycle detected

    // --- Output buffer slot layout (one slot per node output channel) ---
    s.output_slot_base.resize(n);
    size_t slot = 0;
    for (size_t i = 0; i < n; ++i) {
        s.output_slot_base[i] = slot;
        slot += bp.nodes[i]->outputs_count();
    }
    s.total_output_slots = slot;

    // --- Per-instance state block layout ---
    s.state_offset.resize(n);
    s.state_bytes.resize(n);
    size_t off = 0;
    for (size_t i = 0; i < n; ++i) {
        const size_t sz = bp.nodes[i]->state_size();
        const size_t al = bp.nodes[i]->state_align();
        if (sz > 0) {
            off = (off + (al - 1)) & ~(al - 1);
            s.state_offset[i] = off;
            off += sz;
        } else {
            s.state_offset[i] = 0;
        }
        s.state_bytes[i] = sz;
    }
    s.total_state_bytes = off;

    // --- Input wiring: resolve each input channel to a source slot ---
    s.input_source.resize(n);
    for (size_t i = 0; i < n; ++i) {
        s.input_source[i].assign(bp.nodes[i]->inputs_count(), SILENT_SLOT);
    }
    for (const auto& c : bp.connections) {
        s.input_source[c.dst_node][c.dst_input] =
            static_cast<long>(s.output_slot_base[c.src_node] + c.src_output);
    }

    // --- Control-port layout + modulation wiring ---
    s.param_base.resize(n);
    s.param_source.resize(n);
    size_t p = 0;
    for (size_t i = 0; i < n; ++i) {
        s.param_base[i] = p;
        const size_t pc = bp.nodes[i]->params_count();
        s.param_source[i].assign(pc, SILENT_SLOT);
        p += pc;
    }
    s.total_params = p;
    for (const auto& c : bp.param_connections) {
        s.param_source[c.dst_node][c.dst_param] =
            static_cast<long>(s.output_slot_base[c.src_node] + c.src_output);
    }

    s.ok = true;
    return s;
}

} // namespace xrune
