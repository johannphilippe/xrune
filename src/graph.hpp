#pragma once
#include "core.hpp"
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <queue>
#include <algorithm>
#include <atomic>

namespace xrune {

struct connection {
    node* source = nullptr;
    size_t source_output = 0;
    node* target = nullptr;
    size_t target_input = 0;
};

struct executable_task {
    node* n = nullptr;
    node_processing_context context;
    std::atomic<int> remaining_dependencies{0};
    int initial_dependencies = 0;
    std::vector<executable_task*> downstream_tasks;
};

struct graph {
    std::vector<node*> nodes;
    std::vector<connection> connections;
    std::vector<node*> schedule;
    std::vector<std::vector<std::vector<sample_t>>> output_buffers;
    std::vector<sample_t> silent_buffer;
    std::unordered_map<node*, size_t> node_to_index;
    node* input_node = nullptr;
    node* output_node = nullptr;
    bool finished_flag = false;

    // Parallel executable tasks
    std::vector<executable_task*> tasks;
    bool run_parallel = false;

    graph() = default;

    ~graph() {
        clear_tasks();
    }

    void clear_tasks() {
        for (auto* t : tasks) {
            delete t;
        }
        tasks.clear();
    }

    void add_node(node* n) {
        if (!n) return;
        if (node_to_index.find(n) == node_to_index.end()) {
            node_to_index[n] = nodes.size();
            nodes.push_back(n);
        }
    }

    bool connect(node* source, size_t source_output, node* target, size_t target_input) {
        if (!source || !target) return false;
        
        add_node(source);
        add_node(target);

        if (source_output >= source->outputs_count()) return false;
        if (target_input >= target->inputs_count()) return false;

        for (const auto& conn : connections) {
            if (conn.target == target && conn.target_input == target_input) {
                return false;
            }
        }

        connections.push_back({source, source_output, target, target_input});
        return true;
    }

    bool compile() {
        schedule.clear();
        clear_tasks();
        
        size_t n_nodes = nodes.size();
        std::vector<size_t> in_degree(n_nodes, 0);
        std::vector<std::vector<size_t>> adj(n_nodes);

        for (const auto& conn : connections) {
            size_t u = node_to_index[conn.source];
            size_t v = node_to_index[conn.target];
            adj[u].push_back(v);
            in_degree[v]++;
        }

        std::queue<size_t> q;
        for (size_t i = 0; i < n_nodes; ++i) {
            if (in_degree[i] == 0) {
                q.push(i);
            }
        }

        while (!q.empty()) {
            size_t u = q.front();
            q.pop();
            schedule.push_back(nodes[u]);

            for (size_t v : adj[u]) {
                in_degree[v]--;
                if (in_degree[v] == 0) {
                    q.push(v);
                }
            }
        }

        if (schedule.size() != n_nodes) {
            schedule.clear();
            return false;
        }

        // Build parallel executable tasks
        tasks.resize(n_nodes);
        for (size_t i = 0; i < n_nodes; ++i) {
            tasks[i] = new executable_task();
            tasks[i]->n = nodes[i];
        }

        for (const auto& conn : connections) {
            size_t src_idx = node_to_index[conn.source];
            size_t tgt_idx = node_to_index[conn.target];
            tasks[src_idx]->downstream_tasks.push_back(tasks[tgt_idx]);
            tasks[tgt_idx]->initial_dependencies++;
        }

        // Hybrid scheduling check: run parallel if 4 or more nodes
        run_parallel = (n_nodes >= 4);

        return true;
    }

    void prepare_buffers(size_t block_size) {
        output_buffers.resize(nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i) {
            node* n = nodes[i];
            size_t out_count = n->outputs_count();
            output_buffers[i].resize(out_count);
            for (size_t j = 0; j < out_count; ++j) {
                output_buffers[i][j].assign(block_size, 0.0);
            }
        }

        silent_buffer.assign(block_size, 0.0);

        // Pre-configure contexts in executable_tasks
        for (size_t i = 0; i < nodes.size(); ++i) {
            node* n = nodes[i];
            executable_task* t = tasks[i];
            
            t->context.block_size = block_size;
            
            t->context.outputs.resize(n->outputs_count());
            for (size_t ch = 0; ch < n->outputs_count(); ++ch) {
                t->context.outputs[ch] = audio_buffer_view(output_buffers[i][ch].data(), block_size);
            }

            t->context.inputs.resize(n->inputs_count());
            for (size_t ch = 0; ch < n->inputs_count(); ++ch) {
                connection* found_conn = nullptr;
                for (auto& conn : connections) {
                    if (conn.target == n && conn.target_input == ch) {
                        found_conn = &conn;
                        break;
                    }
                }

                if (found_conn) {
                    node* src_node = found_conn->source;
                    size_t src_idx = node_to_index[src_node];
                    size_t src_out = found_conn->source_output;
                    t->context.inputs[ch] = audio_buffer_view(output_buffers[src_idx][src_out].data(), block_size);
                } else {
                    t->context.inputs[ch] = audio_buffer_view(const_cast<sample_t*>(silent_buffer.data()), block_size);
                }
            }
        }
    }

    // Single-threaded fallback process
    void process_block(size_t block_size, size_t sample_rate) {
        if (silent_buffer.size() != block_size) {
            prepare_buffers(block_size);
        }

        for (size_t i = 0; i < nodes.size(); ++i) {
            executable_task* t = tasks[i];
            t->context.sample_rate = sample_rate;
            t->n->process(t->context);
        }
    }
};

} // namespace xrune
