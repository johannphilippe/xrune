#pragma once
#include "core.hpp"
#include <vector>
#include <memory>
#include <utility>
#include <string>

namespace xrune {

struct blueprint_connection {
    size_t src_node = 0;
    size_t src_output = 0;
    size_t dst_node = 0;
    size_t dst_input = 0;
};

// Connection driving a node's control port from another node's output
// (audio-rate modulation: LFO/envelope -> port).
struct param_connection {
    size_t src_node = 0;
    size_t src_output = 0;
    size_t dst_node = 0;
    size_t dst_param = 0;
};

// A named bus exposed by the blueprint. An output terminal reads a node's
// outputs (the instance's signal out); an input terminal points at a bus_input
// node whose outputs the engine fills from routed upstream instances.
struct terminal {
    std::string name;
    size_t node = 0;
};

// Immutable topology description (pre_roadmap §2). Owns its node *types*
// (stateless code + default params); many instances share one blueprint.
// Nodes are referenced by index returned from add().
struct graph_blueprint {
    std::vector<std::unique_ptr<node>> nodes;
    std::vector<std::string> names;   // optional, parallel to nodes ("" = unnamed)
    std::vector<blueprint_connection> connections;
    std::vector<param_connection> param_connections;
    std::vector<terminal> input_terminals;
    std::vector<terminal> output_terminals;
    long input_node = -1;
    long output_node = -1;

    template <typename T, typename... Args>
    size_t add(Args&&... args) {
        nodes.push_back(std::make_unique<T>(std::forward<Args>(args)...));
        names.emplace_back();
        return nodes.size() - 1;
    }

    // Named variant: lets control code / Idyl address the node by name, resolved
    // to an index at compile time (audio thread only ever uses the index).
    template <typename T, typename... Args>
    size_t add_named(const std::string& name, Args&&... args) {
        size_t idx = add<T>(std::forward<Args>(args)...);
        names[idx] = name;
        return idx;
    }

    // Returns the node index for a name, or -1 if not found.
    long find_node(const std::string& name) const {
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == name) return static_cast<long>(i);
        return -1;
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

    size_t add_output_terminal(const std::string& name, size_t node) {
        output_terminals.push_back({name, node});
        return output_terminals.size() - 1;
    }
    size_t add_input_terminal(const std::string& name, size_t node) {
        input_terminals.push_back({name, node});
        return input_terminals.size() - 1;
    }
    long find_output_terminal(const std::string& name) const {
        for (size_t i = 0; i < output_terminals.size(); ++i)
            if (output_terminals[i].name == name) return static_cast<long>(i);
        return -1;
    }
    long find_input_terminal(const std::string& name) const {
        for (size_t i = 0; i < input_terminals.size(); ++i)
            if (input_terminals[i].name == name) return static_cast<long>(i);
        return -1;
    }

    // set_output also registers a default output terminal "out" (terminal 0) so
    // simple blueprints route to the master without any extra wiring.
    void set_output(size_t n) {
        output_node = static_cast<long>(n);
        if (output_terminals.empty()) add_output_terminal("out", n);
    }
    void set_input(size_t n) { input_node = static_cast<long>(n); }

    // Drive dst.param[param] with src.output[out] (audio-rate modulation).
    // Rejects out-of-range ports and a second source on the same port.
    bool connect_param(size_t src, size_t out, size_t dst, size_t param) {
        if (src >= nodes.size() || dst >= nodes.size()) return false;
        if (out >= nodes[src]->outputs_count()) return false;
        if (param >= nodes[dst]->params_count()) return false;
        for (const auto& c : param_connections)
            if (c.dst_node == dst && c.dst_param == param) return false;
        param_connections.push_back({src, out, dst, param});
        return true;
    }

    // Resolve a node's control port by name (compile-time addressing for Idyl).
    long find_param(size_t node_index, const std::string& name) const {
        if (node_index >= nodes.size()) return -1;
        const node* n = nodes[node_index].get();
        const port_descriptor* p = n->params();
        for (size_t i = 0; i < n->params_count(); ++i)
            if (name == p[i].name) return static_cast<long>(i);
        return -1;
    }

    node* node_at(size_t i) const { return nodes[i].get(); }
    size_t size() const { return nodes.size(); }
};

} // namespace xrune
