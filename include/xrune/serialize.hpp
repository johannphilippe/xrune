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

// Blueprint serialization & graph export (roadmap 9b, step 2).
//
//   to_json(bp)   -> a complete, reloadable description of a graph_blueprint
//   from_json(..) -> rebuilds a graph_blueprint, constructing nodes through the
//                    node registry (so any node registered for the language — Faust and
//                    future host nodes included — is loadable for free)
//   to_dot(bp)    -> Graphviz DOT for visual inspection (export only)
//
// All CONTROL-THREAD code: it allocates, and must never run on the audio thread.
//
// What makes the round-trip work: a node reports its type via type_name(), its
// ports via params()/param_default(), and any constructor arguments that are not
// ports (a mixer's input count, an FFT size) via config_args(). Together those
// are exactly what a registry factory needs to rebuild it.
//
// Implementation: src/serialize.cpp

#include "xrune/core.hpp"
#include "xrune/blueprint.hpp"
#include "xrune/util/json.hpp"
#include "xrune/lang/node_registry.hpp"

#include <string>

namespace xrune {

inline constexpr int serialize_format_version = 1;

// ---- JSON export ----------------------------------------------------------
json::value to_json_value(const graph_blueprint& bp, const std::string& name = "");
std::string to_json(const graph_blueprint& bp, const std::string& name = "", int indent = 2);

// ---- JSON import ----------------------------------------------------------
// Rebuilds `out` from JSON text. Nodes are constructed via `reg`, so an
// unregistered type is a hard error (reported, never silently dropped). Returns
// false and fills `err`; `out` is then left incomplete.
bool from_json(const std::string& text,
               const lang::node_registry& reg,
               graph_blueprint& out,
               std::string& err,
               std::string* name_out = nullptr);

// ---- Graphviz DOT export --------------------------------------------------
// Audio connections are solid edges; port modulation (an audio-rate signal
// driving a control port) is a dashed edge labelled with the port name; rate-
// changing nodes are highlighted as region boundaries.
//
//   dot -Tsvg patch.dot -o patch.svg
std::string to_dot(const graph_blueprint& bp, const std::string& name = "xrune");

} // namespace xrune
