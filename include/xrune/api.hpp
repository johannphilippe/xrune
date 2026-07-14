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

// The Xrune control API — a thin, name-addressed facade over the
// blueprint/instance runtime, for external software (Idyl, editors, tests):
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
// Names are resolved to indices on the control thread (once, or cached in a
// param_ref); the audio thread only ever sees integers and generation-checked
// handles. The Xrune language compiles down to exactly these calls.
//
// Declarations only. Implementation: src/api.cpp
// (blueprint_builder::add<T> is the one exception — it is a template, so its
// definition must stay visible here.)

#include "xrune/engine.hpp"

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace xrune {

using blueprint_id = uint32_t;
inline constexpr blueprint_id invalid_blueprint = 0xFFFFFFFFu;

// A spawned instance, as the API user sees it. Carries its blueprint so names
// resolve without extra bookkeeping. Value type, freely copyable.
struct voice {
    instance_handle handle{};
    blueprint_id blueprint = invalid_blueprint;
    bool valid() const { return handle.valid() && blueprint != invalid_blueprint; }
};
inline constexpr voice no_voice{};

// Pre-resolved parameter address, for tight automation loops: resolve once, set
// many times with no string lookup.
struct param_ref {
    size_t node = 0;
    size_t port = 0;
    bool ok = false;
};

// ---- Introspection (what Idyl queries to learn what it can control) --------
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
// A rune parameter, as the host sees it: the name the user actually WROTE
// (`tone.f`), not the node the value happened to be folded into.
//
// `partial` is the honest caveat. A rune parameter is compile-time: it can only
// follow the ports it was passed to *directly* (`sine(freq = f)`). If it was also
// used inside an expression (`sine(freq = f * 2)`), that use was folded to a
// constant and CANNOT follow the parameter at run time. partial == true says so,
// instead of letting a host discover the inconsistency by ear.
// `targets == 0` means the parameter drives nothing at all.
struct rune_param_info {
    std::string name;
    sample_t default_value = 0.0;
    size_t targets = 0;      // how many ports it drives
    bool partial = false;    // also used inside an expression (folded)
};

struct blueprint_info {
    std::string name;
    std::vector<node_info> nodes;
    std::vector<rune_param_info> params;   // the rune's own parameters
    std::vector<std::string> input_terminals;
    std::vector<std::string> output_terminals;
};

// ============================================================================
// Fluent blueprint builder. Everything is addressed by node name; errors
// accumulate (check ok()/errors) and register_blueprint refuses a builder that
// has any.
//
// The builder owns its nodes (unique_ptr), so it is MOVE-ONLY: bind the chain to
// a named blueprint_builder, or pass it straight to register_blueprint().
// ============================================================================
struct blueprint_builder {
    std::string name;
    graph_blueprint bp;
    std::vector<std::string> errors;

    explicit blueprint_builder(std::string n = "") : name(std::move(n)) {}

    // Template: must be defined in the header.
    template <typename T, typename... Args>
    blueprint_builder& add(const std::string& node_name, Args&&... args) {
        if (node_name.empty()) { errors.push_back("node name must not be empty"); return *this; }
        if (bp.find_node(node_name) >= 0) { errors.push_back("duplicate node name: " + node_name); return *this; }
        bp.add_named<T>(node_name, std::forward<Args>(args)...);
        return *this;
    }

    // Audio connection: src.output[out] -> dst.input[in].
    blueprint_builder& connect(const std::string& src, size_t out,
                               const std::string& dst, size_t in);

    // Audio-rate modulation: src.output[out] -> dst.port(name).
    blueprint_builder& modulate(const std::string& src, size_t out,
                                const std::string& dst, const std::string& port);

    // Designate the output node (auto-registers the output terminal "out" if
    // none exists yet).
    blueprint_builder& output(const std::string& node_name);

    // Named terminals, for cross-instance routing.
    blueprint_builder& input_terminal(const std::string& terminal_name, const std::string& node_name);
    blueprint_builder& output_terminal(const std::string& terminal_name, const std::string& node_name);

    bool ok() const { return errors.empty(); }
};

// Entry point for the fluent chain: xrune::build("name").add<...>(...)…
blueprint_builder build(std::string name);

// ============================================================================
// Runtime facade: blueprint registry + engine. CONTROL-THREAD ONLY.
// ============================================================================
struct runtime_config {
    size_t sample_rate = 48000;
    size_t block_size = 128;     // must be a power of two
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

    // Rune-parameter values for THIS voice, applied before it runs. Use these to
    // spawn a note at a pitch: the voice starts AT the value. Setting the port
    // after spawn instead would ramp from the compiled default over the first
    // block -- correct smoothing for a live change, but a chirp on a note-on.
    std::vector<std::pair<std::string, sample_t>> params;
};

struct runtime {
    // A registered blueprint: topology, its compiled schedule, and the
    // introspection record built once at registration.
    struct registered {
        std::string name;
        graph_blueprint bp;
        compiled_schedule sched;
        blueprint_info info;
    };

    // Declared before the engine so it is destroyed *after* it: live instances
    // point into these blueprints and schedules.
    std::vector<std::unique_ptr<registered>> registry;
    engine eng;
    runtime_config cfg;
    bool inited = false;
    std::string err;

    // ---- Lifecycle ----

    // Inject a backend (e.g. offline_backend for tests) before init().
    void use_backend(std::unique_ptr<audio_backend> b);

    // Fails if the block size is not a power of two (the 64-byte buffer
    // alignment guarantee depends on it), or if the backend cannot open.
    bool init(const runtime_config& c = {});

    bool start();
    void stop();

    const runtime_config& config() const { return cfg; }
    const std::string& last_error() const { return err; }

    // ---- Blueprints ----

    // Compiles and stores the blueprint, returning its id (invalid_blueprint on
    // error — see last_error()). The builder's content is moved out.
    blueprint_id register_blueprint(blueprint_builder& b);
    blueprint_id register_blueprint(blueprint_builder&& b);

    blueprint_id find_blueprint(const std::string& name) const;

    // What nodes / ports / terminals a blueprint exposes.
    const blueprint_info* describe(blueprint_id id) const;

    // ---- Voices ----

    voice spawn(blueprint_id id, const spawn_options& opt = {});
    void kill(const voice& v);

    // True while the handle refers to a live (not yet reclaimed) instance. An
    // auto-reaped voice becomes false after the next pump().
    bool alive(const voice& v) const;

    // Recycle finished/killed instances (call periodically from the control
    // loop). Returns how many were reclaimed.
    size_t pump();

    size_t active_voices() const;

    // ---- Parameters ----

    param_ref resolve(blueprint_id id, const std::string& node, const std::string& port);
    bool set(const voice& v, param_ref p, sample_t value);
    bool set(const voice& v, const std::string& node, const std::string& port, sample_t value);

    // Set a RUNE parameter by the name the user wrote (`rt.set_param(v, "f", 440)`).
    // Fans out to every port that parameter drives. Fails if the name is unknown
    // or drives nothing. Smoothed, like any other control-rate change.
    bool set_param(const voice& v, const std::string& name, sample_t value);

    // ---- Routing (runtime rewiring; spawn already routes once) ----

    bool route(const voice& src, const voice& dst,
               const std::string& dst_terminal = "in", const std::string& src_terminal = "out");
    bool unroute(const voice& src, const voice& dst,
                 const std::string& dst_terminal = "in", const std::string& src_terminal = "out");
    bool route_to_master(const voice& src, const std::string& src_terminal = "out");
    bool unroute_from_master(const voice& src, const std::string& src_terminal = "out");

    // ---- Time helpers ----

    size_t blocks(double seconds) const;
    lifetime_policy for_seconds(double seconds) const;
    lifetime_policy until_silent_for(double seconds, sample_t threshold = 1e-5) const;

private:
    registered* get(blueprint_id id);
    long resolve_out_term(const voice& src, const std::string& name);
    bool resolve_route(const voice& src, const voice& dst,
                       const std::string& dst_terminal, const std::string& src_terminal,
                       size_t& src_term_out, route_target& out);
    static blueprint_info make_info(const registered& r);
};

} // namespace xrune
