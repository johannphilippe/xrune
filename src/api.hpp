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
#include "engine.hpp"
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// Phase 9: the Xrune control API. A thin, name-addressed facade for external
// software (Idyl, editors, tests) over the blueprint/instance runtime:
//
//   xrune::runtime rt;
//   rt.init({48000, 128});
//   auto id = rt.register_blueprint(
//       xrune::build("voice")
//           .add<oscillator>("osc", 440.0)
//           .add<gain>("amp", 0.25)
//           .connect("osc", 0, "amp", 0)
//           .output("amp"));
//   rt.start();
//   xrune::voice v = rt.spawn(id);
//   rt.set(v, "osc", "freq", 220.0);
//   rt.kill(v);
//
// Names are resolved to indices on the control thread (once, or cached via
// param_ref); the audio thread only ever sees integers and generation-checked
// handles. The DSL (later) compiles down to exactly these calls.

namespace xrune {

using blueprint_id = uint32_t;
inline constexpr blueprint_id invalid_blueprint = 0xFFFFFFFFu;

// A spawned instance, as seen by the API user. Carries its blueprint so names
// can be resolved without extra bookkeeping. Value type, freely copyable.
struct voice {
    instance_handle handle{};
    blueprint_id blueprint = invalid_blueprint;
    bool valid() const { return handle.valid() && blueprint != invalid_blueprint; }
};
inline constexpr voice no_voice{};

// Pre-resolved parameter address (for tight automation loops: resolve once,
// set many times without string lookups).
struct param_ref {
    size_t node = 0;
    size_t port = 0;
    bool ok = false;
};

// ---- Introspection (what Idyl queries to know what it can control) ----
struct port_info {
    std::string name;
    sample_t default_value = 0.0;
    sample_t min_value = 0.0;
    sample_t max_value = 0.0;
};
struct node_info {
    std::string name;
    size_t index = 0;
    size_t inputs = 0;
    size_t outputs = 0;
    std::vector<port_info> ports;
};
struct blueprint_info {
    std::string name;
    std::vector<node_info> nodes;
    std::vector<std::string> input_terminals;
    std::vector<std::string> output_terminals;
};

// ============================================================================
// Fluent blueprint builder. All references are by node name; errors accumulate
// (check ok()/errors) and register_blueprint refuses a builder with errors.
// ============================================================================
struct blueprint_builder {
    std::string name;
    graph_blueprint bp;
    std::vector<std::string> errors;

    explicit blueprint_builder(std::string n = "") : name(std::move(n)) {}

    template <typename T, typename... Args>
    blueprint_builder& add(const std::string& node_name, Args&&... args) {
        if (node_name.empty()) { errors.push_back("node name must not be empty"); return *this; }
        if (bp.find_node(node_name) >= 0) { errors.push_back("duplicate node name: " + node_name); return *this; }
        bp.add_named<T>(node_name, std::forward<Args>(args)...);
        return *this;
    }

    // Audio connection: src.output[out] -> dst.input[in].
    blueprint_builder& connect(const std::string& src, size_t out,
                               const std::string& dst, size_t in) {
        long s = bp.find_node(src), d = bp.find_node(dst);
        if (s < 0) { errors.push_back("connect: unknown node: " + src); return *this; }
        if (d < 0) { errors.push_back("connect: unknown node: " + dst); return *this; }
        if (!bp.connect(static_cast<size_t>(s), out, static_cast<size_t>(d), in))
            errors.push_back("connect: invalid (" + src + " -> " + dst + "): port out of range or input already driven");
        return *this;
    }

    // Audio-rate modulation: src.output[out] -> dst.port(name).
    blueprint_builder& modulate(const std::string& src, size_t out,
                                const std::string& dst, const std::string& port) {
        long s = bp.find_node(src), d = bp.find_node(dst);
        if (s < 0) { errors.push_back("modulate: unknown node: " + src); return *this; }
        if (d < 0) { errors.push_back("modulate: unknown node: " + dst); return *this; }
        long p = bp.find_param(static_cast<size_t>(d), port);
        if (p < 0) { errors.push_back("modulate: unknown port: " + dst + "." + port); return *this; }
        if (!bp.connect_param(static_cast<size_t>(s), out, static_cast<size_t>(d), static_cast<size_t>(p)))
            errors.push_back("modulate: invalid (" + src + " -> " + dst + "." + port + "): out of range or port already driven");
        return *this;
    }

    // Designate the output node (auto-registers output terminal "out" if none).
    blueprint_builder& output(const std::string& node_name) {
        long n = bp.find_node(node_name);
        if (n < 0) { errors.push_back("output: unknown node: " + node_name); return *this; }
        bp.set_output(static_cast<size_t>(n));
        return *this;
    }

    // Named terminals for cross-instance routing.
    blueprint_builder& input_terminal(const std::string& terminal_name, const std::string& node_name) {
        long n = bp.find_node(node_name);
        if (n < 0) { errors.push_back("input_terminal: unknown node: " + node_name); return *this; }
        if (bp.find_input_terminal(terminal_name) >= 0) { errors.push_back("duplicate input terminal: " + terminal_name); return *this; }
        bp.add_input_terminal(terminal_name, static_cast<size_t>(n));
        return *this;
    }
    blueprint_builder& output_terminal(const std::string& terminal_name, const std::string& node_name) {
        long n = bp.find_node(node_name);
        if (n < 0) { errors.push_back("output_terminal: unknown node: " + node_name); return *this; }
        if (bp.find_output_terminal(terminal_name) >= 0) { errors.push_back("duplicate output terminal: " + terminal_name); return *this; }
        bp.add_output_terminal(terminal_name, static_cast<size_t>(n));
        return *this;
    }

    bool ok() const { return errors.empty(); }
};

// Entry point for the fluent chain: xrune::build("name").add<...>(...)...
inline blueprint_builder build(std::string name) { return blueprint_builder(std::move(name)); }

// ============================================================================
// Runtime facade: blueprint registry + engine. Control-thread only.
// ============================================================================
struct runtime_config {
    size_t sample_rate = 48000;
    size_t block_size = 128;
    size_t input_channels = 0;
    size_t output_channels = 2;
    size_t max_instances = 128;
    size_t workers = 0;          // 0 = single-threaded executor
};

struct spawn_options {
    lifetime_policy life{};              // default: permanent
    voice into = no_voice;               // invalid -> route to master
    std::string into_terminal = "in";    // destination input terminal (by name)
    std::string from_terminal = "out";   // this voice's output terminal (by name)
};

struct runtime {
    // Registry is declared before the engine so it is destroyed after it:
    // live instances point into blueprints/schedules.
    struct registered {
        std::string name;
        graph_blueprint bp;
        compiled_schedule sched;
        blueprint_info info;
    };
    std::vector<std::unique_ptr<registered>> registry;
    engine eng;
    runtime_config cfg;
    bool inited = false;
    std::string err;

    // ---- Lifecycle ----

    // Inject a backend (e.g. offline_backend for tests) before init().
    void use_backend(std::unique_ptr<audio_backend> b) { eng.use_backend(std::move(b)); }

    bool init(const runtime_config& c = {}) {
        cfg = c;
        if (cfg.block_size == 0 || (cfg.block_size & (cfg.block_size - 1)) != 0) {
            err = "block size must be a power of two (got " + std::to_string(cfg.block_size) + ")";
            inited = false;
            return false;
        }
        inited = eng.init(cfg.sample_rate, cfg.block_size, cfg.input_channels,
                          cfg.output_channels, cfg.max_instances, cfg.workers);
        if (!inited) err = "engine init failed (audio backend)";
        return inited;
    }

    bool start() { return eng.start(); }
    void stop() { eng.stop(); }

    const runtime_config& config() const { return cfg; }
    const std::string& last_error() const { return err; }

    // ---- Blueprints ----

    // Compiles and stores the blueprint; returns its id (invalid_blueprint on
    // error, see last_error()). The builder's content is moved out.
    blueprint_id register_blueprint(blueprint_builder& b) {
        if (!inited) { err = "register_blueprint: runtime not initialized (block size unknown)"; return invalid_blueprint; }
        if (!b.ok()) {
            err = "register_blueprint(" + b.name + "): ";
            for (size_t i = 0; i < b.errors.size(); ++i) err += (i ? "; " : "") + b.errors[i];
            return invalid_blueprint;
        }
        auto reg = std::make_unique<registered>();
        reg->name = b.name;
        reg->bp = std::move(b.bp);
        reg->sched = compile(reg->bp, cfg.block_size);
        if (!reg->sched.ok) {
            err = "register_blueprint(" + b.name + "): compile failed (cycle, or invalid rate/block configuration)";
            return invalid_blueprint;
        }
        reg->info = make_info(*reg);
        registry.push_back(std::move(reg));
        return static_cast<blueprint_id>(registry.size() - 1);
    }
    blueprint_id register_blueprint(blueprint_builder&& b) { return register_blueprint(b); }

    blueprint_id find_blueprint(const std::string& name) const {
        for (size_t i = 0; i < registry.size(); ++i)
            if (registry[i]->name == name) return static_cast<blueprint_id>(i);
        return invalid_blueprint;
    }

    // Introspection: what nodes/ports/terminals a blueprint exposes.
    const blueprint_info* describe(blueprint_id id) const {
        return (id < registry.size()) ? &registry[id]->info : nullptr;
    }

    // ---- Voices ----

    voice spawn(blueprint_id id, const spawn_options& opt = {}) {
        registered* r = get(id);
        if (!r) return no_voice;

        size_t src_term = 0;
        if (!r->bp.output_terminals.empty()) {
            long t = r->bp.find_output_terminal(opt.from_terminal);
            if (t < 0) { err = "spawn: unknown output terminal: " + opt.from_terminal; return no_voice; }
            src_term = static_cast<size_t>(t);
        }

        route_target dest = route_target::master();
        if (opt.into.valid()) {
            registered* rd = get(opt.into.blueprint);
            if (!rd) return no_voice;
            long dt = rd->bp.find_input_terminal(opt.into_terminal);
            if (dt < 0) { err = "spawn: unknown input terminal: " + opt.into_terminal; return no_voice; }
            if (!eng.is_valid(opt.into.handle)) { err = "spawn: destination voice is not alive"; return no_voice; }
            dest = route_target::to(opt.into.handle, static_cast<size_t>(dt));
        }

        instance_handle h = eng.spawn(r->sched, opt.life, dest, src_term);
        if (!h.valid()) { err = "spawn: instance pool exhausted"; return no_voice; }
        return voice{h, id};
    }

    void kill(const voice& v) { if (v.valid()) eng.kill(v.handle); }

    // True while the handle refers to a live (not yet reclaimed) instance.
    // Auto-reaped voices become false after the next pump().
    bool alive(const voice& v) const { return v.valid() && eng.is_valid(v.handle); }

    // Recycle finished/killed instances (call periodically from the control
    // loop). Returns how many were reclaimed.
    size_t pump() { return eng.reclaim(); }

    size_t active_voices() const { return eng.active_count(); }

    // ---- Parameters ----

    param_ref resolve(blueprint_id id, const std::string& node, const std::string& port) {
        registered* r = get(id);
        if (!r) return {};
        long n = r->bp.find_node(node);
        if (n < 0) { err = "resolve: unknown node: " + node; return {}; }
        long p = r->bp.find_param(static_cast<size_t>(n), port);
        if (p < 0) { err = "resolve: unknown port: " + node + "." + port; return {}; }
        return {static_cast<size_t>(n), static_cast<size_t>(p), true};
    }

    bool set(const voice& v, param_ref p, sample_t value) {
        if (!v.valid() || !p.ok) { err = "set: invalid voice or param"; return false; }
        eng.set_parameter(v.handle, p.node, p.port, value);
        return true;
    }

    bool set(const voice& v, const std::string& node, const std::string& port, sample_t value) {
        param_ref p = resolve(v.blueprint, node, port);
        return p.ok ? set(v, p, value) : false;
    }

    // ---- Routing (runtime rewiring; spawn already routes once) ----

    bool route(const voice& src, const voice& dst,
               const std::string& dst_terminal = "in", const std::string& src_terminal = "out") {
        size_t st = 0; route_target t{};
        if (!resolve_route(src, dst, dst_terminal, src_terminal, st, t)) return false;
        eng.connect(src.handle, st, t);
        return true;
    }
    bool unroute(const voice& src, const voice& dst,
                 const std::string& dst_terminal = "in", const std::string& src_terminal = "out") {
        size_t st = 0; route_target t{};
        if (!resolve_route(src, dst, dst_terminal, src_terminal, st, t)) return false;
        eng.disconnect(src.handle, st, t);
        return true;
    }
    bool route_to_master(const voice& src, const std::string& src_terminal = "out") {
        long st = resolve_out_term(src, src_terminal);
        if (st < 0) return false;
        eng.connect(src.handle, static_cast<size_t>(st), route_target::master());
        return true;
    }
    bool unroute_from_master(const voice& src, const std::string& src_terminal = "out") {
        long st = resolve_out_term(src, src_terminal);
        if (st < 0) return false;
        eng.disconnect(src.handle, static_cast<size_t>(st), route_target::master());
        return true;
    }

    // ---- Time helpers ----

    size_t blocks(double seconds) const {
        return static_cast<size_t>(seconds * static_cast<double>(cfg.sample_rate)
                                   / static_cast<double>(cfg.block_size) + 0.5);
    }
    lifetime_policy for_seconds(double seconds) const {
        return {lifetime_kind::timed, blocks(seconds), 1e-5, 0};
    }
    lifetime_policy until_silent_for(double seconds, sample_t threshold = 1e-5) const {
        return {lifetime_kind::until_silent, 0, threshold, blocks(seconds)};
    }

private:
    registered* get(blueprint_id id) {
        if (id >= registry.size()) { err = "unknown blueprint id"; return nullptr; }
        return registry[id].get();
    }

    long resolve_out_term(const voice& src, const std::string& name) {
        registered* r = get(src.blueprint);
        if (!r || !src.valid()) { err = "route: invalid source voice"; return -1; }
        long t = r->bp.find_output_terminal(name);
        if (t < 0) err = "route: unknown output terminal: " + name;
        return t;
    }

    bool resolve_route(const voice& src, const voice& dst,
                       const std::string& dst_terminal, const std::string& src_terminal,
                       size_t& src_term_out, route_target& out) {
        long st = resolve_out_term(src, src_terminal);
        if (st < 0) return false;
        registered* rd = get(dst.blueprint);
        if (!rd || !dst.valid()) { err = "route: invalid destination voice"; return false; }
        long dt = rd->bp.find_input_terminal(dst_terminal);
        if (dt < 0) { err = "route: unknown input terminal: " + dst_terminal; return false; }
        src_term_out = static_cast<size_t>(st);
        out = route_target::to(dst.handle, static_cast<size_t>(dt));
        return true;
    }

    static blueprint_info make_info(const registered& r) {
        blueprint_info info;
        info.name = r.name;
        for (size_t i = 0; i < r.bp.size(); ++i) {
            const node* n = r.bp.node_at(i);
            node_info ni;
            ni.name = r.bp.names[i];
            ni.index = i;
            ni.inputs = n->inputs_count();
            ni.outputs = n->outputs_count();
            const port_descriptor* pd = n->params();
            for (size_t p = 0; p < n->params_count(); ++p)
                ni.ports.push_back({pd[p].name, n->param_default(p), pd[p].min_value, pd[p].max_value});
            info.nodes.push_back(std::move(ni));
        }
        for (const auto& t : r.bp.input_terminals) info.input_terminals.push_back(t.name);
        for (const auto& t : r.bp.output_terminals) info.output_terminals.push_back(t.name);
        return info;
    }
};

} // namespace xrune
