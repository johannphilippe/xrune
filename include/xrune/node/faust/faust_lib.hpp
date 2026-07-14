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

// faustlib — reach any function of the Faust standard libraries from Xrune:
//
//     rune f
//       smoother = faustlib("si.smoo")
//       filter   = faustlib("ve.korg35LPF")          // ports: normFreq, Q
//       lp       = faustlib("fi.lowpass", N = 3)     // N is COMPILE-TIME
//     end
//
// It writes the Faust source for you, JIT-compiles it, and caches the factory:
//
//     import("stdfaust.lib");
//     process = ve.korg35LPF(hslider("normFreq", 0.35, 0, 1, 0.001),
//                            hslider("Q",        3.5, 0.7, 10, 0.1));
//
// Two things Xrune deliberately does NOT guess:
//
// * **Audio arity and the port list** come from libfaust itself
//   (getNumInputs/getNumOutputs + APIUI), never from parsing documentation.
//   faust_node_base already does this, so a faustlib node has correct I/O by
//   construction.
//
// * **Compile-time arguments** (a filter order N, a table size) cannot be
//   sliders — `fi.lowpass(hslider(...), ...)` simply does not compile. The
//   metadata marks them, and you must pass a literal:  faustlib("fi.lowpass", N = 3).
//   Omitting one is a clear error, not a mysterious Faust compiler message.
//
// The parameter NAMES and their ranges come from an index of the Faust libraries
// produced by tools/faustlib_scan.py (see faustlib_index).

#include "xrune/node/faust/faust_jit.hpp"
#include "xrune/util/json.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace xrune {

// One argument of a Faust library function, as mined from the library docs.
struct faustlib_param {
    std::string name;
    std::string doc;
    bool is_const = false;      // a compile-time literal: cannot be a slider
    bool from_test = false;     // range is the library author's, not a fallback
    double default_value = 0.0;
    double min_value = -1e6;
    double max_value = 1e6;
    double step = 0.001;
};

struct faustlib_function {
    std::string name;                      // "ve.korg35LPF"
    std::string lib;                       // "vaeffects.lib"
    std::vector<faustlib_param> params;
};

// The index of the Faust standard libraries. Load once (from the JSON produced
// by tools/faustlib_scan.py) and share.
struct faustlib_index {
    std::map<std::string, faustlib_function> functions;

    bool load(const std::string& path, std::string& err);
    bool load_json(const std::string& text, std::string& err);

    const faustlib_function* find(const std::string& name) const {
        auto it = functions.find(name);
        return it == functions.end() ? nullptr : &it->second;
    }

    // Search the usual places: $XRUNE_FAUSTLIB_JSON, then the installed
    // share/xrune/faustlib.json, then ./data/faustlib.json.
    static faustlib_index& standard();
};

// Generate the Faust source for one library function.
//
// `overrides` supplies compile-time arguments (required for `const` params) and
// may also replace a control parameter's default. Throws std::runtime_error with
// an actionable message if a compile-time argument is missing.
std::string faustlib_source(const faustlib_function& fn,
                            const std::map<std::string, double>& overrides);

// Compile-once cache, keyed by the generated source. Asking for the same
// function twice returns the SAME factory, so `faustlib("si.smoo")` used in
// twenty places compiles once. Thread-safe; control thread only.
std::shared_ptr<faust_factory> faustlib_factory(const std::string& source);

// Build a node for a Faust library function. `overrides` as above.
std::unique_ptr<node> make_faustlib(const std::string& function_name,
                                    const std::map<std::string, double>& overrides,
                                    const faustlib_index& index = faustlib_index::standard());

} // namespace xrune
