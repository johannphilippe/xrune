#pragma once
#include "core.hpp"
#include <vector>
#include <memory>
#include <utility>

namespace xrune {

struct blueprint_connection {
    size_t src_node = 0;
    size_t src_output = 0;
    size_t dst_node = 0;
    size_t dst_input = 0;
};

// Immutable topology description (pre_roadmap §2). Owns its node *types*
// (stateless code + default params); many instances share one blueprint.
// Nodes are referenced by index returned from add().
struct graph_blueprint {
    std::vector<std::unique_ptr<node>> nodes;
    std::vector<blueprint_connection> connections;
    long input_node = -1;
    long output_node = -1;

    template <typename T, typename... Args>
    size_t add(Args&&... args) {
        nodes.push_back(std::make_unique<T>(std::forward<Args>(args)...));
        return nodes.size() - 1;
    }

    // Connect src.output -> dst.input. Rejects out-of-range ports, and enforces
    // a single source per input (extra fan-in goes through a mixer node).
    bool connect(size_t src, size_t out, size_t dst, size_t in) {
        if (src >= nodes.size() || dst >= nodes.size()) return false;
        if (out >= nodes[src]->outputs_count()) return false;
        if (in >= nodes[dst]->inputs_count()) return false;
        for (const auto& c : connections)
            if (c.dst_node == dst && c.dst_input == in) return false;
        connections.push_back({src, out, dst, in});
        return true;
    }

    void set_output(size_t n) { output_node = static_cast<long>(n); }
    void set_input(size_t n) { input_node = static_cast<long>(n); }

    node* node_at(size_t i) const { return nodes[i].get(); }
    size_t size() const { return nodes.size(); }
};

} // namespace xrune
