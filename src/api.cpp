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

#include "xrune/api.hpp"

namespace xrune {

// ============================================================================
// blueprint_builder
// ============================================================================

blueprint_builder& blueprint_builder::connect(const std::string& src, size_t out,
                                              const std::string& dst, size_t in) {
    long s = bp.find_node(src), d = bp.find_node(dst);
    if (s < 0) { errors.push_back("connect: unknown node: " + src); return *this; }
    if (d < 0) { errors.push_back("connect: unknown node: " + dst); return *this; }
    if (!bp.connect(static_cast<size_t>(s), out, static_cast<size_t>(d), in))
        errors.push_back("connect: invalid (" + src + " -> " + dst +
                         "): port out of range or input already driven");
    return *this;
}

blueprint_builder& blueprint_builder::modulate(const std::string& src, size_t out,
                                               const std::string& dst, const std::string& port) {
    long s = bp.find_node(src), d = bp.find_node(dst);
    if (s < 0) { errors.push_back("modulate: unknown node: " + src); return *this; }
    if (d < 0) { errors.push_back("modulate: unknown node: " + dst); return *this; }
    long p = bp.find_param(static_cast<size_t>(d), port);
    if (p < 0) { errors.push_back("modulate: unknown port: " + dst + "." + port); return *this; }
    if (!bp.connect_param(static_cast<size_t>(s), out, static_cast<size_t>(d), static_cast<size_t>(p)))
        errors.push_back("modulate: invalid (" + src + " -> " + dst + "." + port +
                         "): out of range or port already driven");
    return *this;
}

blueprint_builder& blueprint_builder::output(const std::string& node_name) {
    long n = bp.find_node(node_name);
    if (n < 0) { errors.push_back("output: unknown node: " + node_name); return *this; }
    bp.set_output(static_cast<size_t>(n));
    return *this;
}

blueprint_builder& blueprint_builder::input_terminal(const std::string& terminal_name,
                                                     const std::string& node_name) {
    long n = bp.find_node(node_name);
    if (n < 0) { errors.push_back("input_terminal: unknown node: " + node_name); return *this; }
    if (bp.find_input_terminal(terminal_name) >= 0) {
        errors.push_back("duplicate input terminal: " + terminal_name);
        return *this;
    }
    bp.add_input_terminal(terminal_name, static_cast<size_t>(n));
    return *this;
}

blueprint_builder& blueprint_builder::output_terminal(const std::string& terminal_name,
                                                      const std::string& node_name) {
    long n = bp.find_node(node_name);
    if (n < 0) { errors.push_back("output_terminal: unknown node: " + node_name); return *this; }
    if (bp.find_output_terminal(terminal_name) >= 0) {
        errors.push_back("duplicate output terminal: " + terminal_name);
        return *this;
    }
    bp.add_output_terminal(terminal_name, static_cast<size_t>(n));
    return *this;
}

blueprint_builder build(std::string name) {
    return blueprint_builder(std::move(name));
}

// ============================================================================
// runtime — lifecycle
// ============================================================================

void runtime::use_backend(std::unique_ptr<audio_backend> b) {
    eng.use_backend(std::move(b));
}

bool runtime::init(const runtime_config& c) {
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

bool runtime::start() { return eng.start(); }
void runtime::stop()  { eng.stop(); }

// ============================================================================
// runtime — blueprints
// ============================================================================

blueprint_id runtime::register_blueprint(blueprint_builder& b) {
    if (!inited) {
        err = "register_blueprint: runtime not initialized (block size unknown)";
        return invalid_blueprint;
    }
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
        err = "register_blueprint(" + b.name +
              "): compile failed (cycle, or invalid rate/block configuration)";
        return invalid_blueprint;
    }
    reg->info = make_info(*reg);
    registry.push_back(std::move(reg));
    return static_cast<blueprint_id>(registry.size() - 1);
}

blueprint_id runtime::register_blueprint(blueprint_builder&& b) {
    return register_blueprint(b);
}

blueprint_id runtime::find_blueprint(const std::string& name) const {
    for (size_t i = 0; i < registry.size(); ++i)
        if (registry[i]->name == name) return static_cast<blueprint_id>(i);
    return invalid_blueprint;
}

const blueprint_info* runtime::describe(blueprint_id id) const {
    return (id < registry.size()) ? &registry[id]->info : nullptr;
}

// ============================================================================
// runtime — voices
// ============================================================================

voice runtime::spawn(blueprint_id id, const spawn_options& opt) {
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

    // Resolve the rune-parameter overrides into concrete port writes. An unknown
    // name, or one that drives no port, is an ERROR: silently ignoring it would
    // mean the voice quietly plays at the wrong pitch.
    std::vector<initial_param> init;
    for (const auto& kv : opt.params) {
        const long pi = r->bp.find_rune_param(kv.first);
        if (pi < 0) {
            err = "spawn: unknown rune parameter '" + kv.first + "'";
            return no_voice;
        }
        const blueprint_param& bpp = r->bp.params[static_cast<size_t>(pi)];
        if (bpp.targets.empty()) {
            err = "spawn: rune parameter '" + kv.first + "' drives no port "
                  "(it is only used inside an expression, so it was folded at "
                  "compile time)";
            return no_voice;
        }
        for (const param_target& tg : bpp.targets)
            init.push_back({tg.node, tg.port, kv.second});
    }

    instance_handle h = eng.spawn(r->sched, opt.life, dest, src_term,
                                  init.empty() ? nullptr : &init);
    if (!h.valid()) { err = "spawn: instance pool exhausted"; return no_voice; }
    return voice{h, id};
}

void runtime::kill(const voice& v) {
    if (v.valid()) eng.kill(v.handle);
}

bool runtime::alive(const voice& v) const {
    return v.valid() && eng.is_valid(v.handle);
}

size_t runtime::pump() { return eng.reclaim(); }

size_t runtime::active_voices() const { return eng.active_count(); }

// ============================================================================
// runtime — parameters
// ============================================================================

param_ref runtime::resolve(blueprint_id id, const std::string& node, const std::string& port) {
    registered* r = get(id);
    if (!r) return {};
    long n = r->bp.find_node(node);
    if (n < 0) { err = "resolve: unknown node: " + node; return {}; }
    long p = r->bp.find_param(static_cast<size_t>(n), port);
    if (p < 0) { err = "resolve: unknown port: " + node + "." + port; return {}; }
    return {static_cast<size_t>(n), static_cast<size_t>(p), true};
}

bool runtime::set(const voice& v, param_ref p, sample_t value) {
    if (!v.valid() || !p.ok) { err = "set: invalid voice or param"; return false; }
    eng.set_parameter(v.handle, p.node, p.port, value);
    return true;
}

bool runtime::set(const voice& v, const std::string& node, const std::string& port, sample_t value) {
    param_ref p = resolve(v.blueprint, node, port);
    return p.ok ? set(v, p, value) : false;
}

bool runtime::set_param(const voice& v, const std::string& name, sample_t value) {
    if (!v.valid()) { err = "set_param: invalid voice"; return false; }
    registered* r = get(v.blueprint);
    if (!r) return false;

    const long pi = r->bp.find_rune_param(name);
    if (pi < 0) { err = "set_param: unknown rune parameter '" + name + "'"; return false; }

    const blueprint_param& bpp = r->bp.params[static_cast<size_t>(pi)];
    if (bpp.targets.empty()) {
        err = "set_param: rune parameter '" + name + "' drives no port";
        return false;
    }
    // Fan out to every port the parameter drives. Smoothed, like any other
    // control-rate change (use spawn_options::params for a glide-free note-on).
    for (const param_target& tg : bpp.targets)
        eng.set_parameter(v.handle, tg.node, tg.port, value);
    return true;
}

// ============================================================================
// runtime — routing
// ============================================================================

bool runtime::route(const voice& src, const voice& dst,
                    const std::string& dst_terminal, const std::string& src_terminal) {
    size_t st = 0;
    route_target t{};
    if (!resolve_route(src, dst, dst_terminal, src_terminal, st, t)) return false;
    eng.connect(src.handle, st, t);
    return true;
}

bool runtime::unroute(const voice& src, const voice& dst,
                      const std::string& dst_terminal, const std::string& src_terminal) {
    size_t st = 0;
    route_target t{};
    if (!resolve_route(src, dst, dst_terminal, src_terminal, st, t)) return false;
    eng.disconnect(src.handle, st, t);
    return true;
}

bool runtime::route_to_master(const voice& src, const std::string& src_terminal) {
    long st = resolve_out_term(src, src_terminal);
    if (st < 0) return false;
    eng.connect(src.handle, static_cast<size_t>(st), route_target::master());
    return true;
}

bool runtime::unroute_from_master(const voice& src, const std::string& src_terminal) {
    long st = resolve_out_term(src, src_terminal);
    if (st < 0) return false;
    eng.disconnect(src.handle, static_cast<size_t>(st), route_target::master());
    return true;
}

// ============================================================================
// runtime — time helpers
// ============================================================================

size_t runtime::blocks(double seconds) const {
    return static_cast<size_t>(seconds * static_cast<double>(cfg.sample_rate)
                               / static_cast<double>(cfg.block_size) + 0.5);
}

lifetime_policy runtime::for_seconds(double seconds) const {
    return {lifetime_kind::timed, blocks(seconds), 1e-5, 0};
}

lifetime_policy runtime::until_silent_for(double seconds, sample_t threshold) const {
    return {lifetime_kind::until_silent, 0, threshold, blocks(seconds)};
}

// ============================================================================
// runtime — private helpers
// ============================================================================

runtime::registered* runtime::get(blueprint_id id) {
    if (id >= registry.size()) { err = "unknown blueprint id"; return nullptr; }
    return registry[id].get();
}

long runtime::resolve_out_term(const voice& src, const std::string& name) {
    registered* r = get(src.blueprint);
    if (!r || !src.valid()) { err = "route: invalid source voice"; return -1; }
    long t = r->bp.find_output_terminal(name);
    if (t < 0) err = "route: unknown output terminal: " + name;
    return t;
}

bool runtime::resolve_route(const voice& src, const voice& dst,
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

blueprint_info runtime::make_info(const registered& r) {
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
    for (const auto& p : r.bp.params)
        info.params.push_back({p.name, p.default_value, p.targets.size(), p.partial});
    for (const auto& t : r.bp.input_terminals)  info.input_terminals.push_back(t.name);
    for (const auto& t : r.bp.output_terminals) info.output_terminals.push_back(t.name);
    return info;
}

} // namespace xrune
