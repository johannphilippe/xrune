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
#include "xrune/lang/parser/parser.hpp"
#include "xrune/lang/lower.hpp"
#include "xrune/lang/node_registry.hpp"
#include "xrune/api.hpp"
#include <string>
#include <vector>
#include <utility>
#include <fstream>
#include <iterator>

// Xrune front-to-back: source text -> parsed AST -> lowered graph_blueprints ->
// registered in a runtime. This is the whole DSL pipeline behind one call;
// Idyl / tooling use it to load .rune files.

namespace xrune::lang {

struct load_result {
    std::vector<std::pair<std::string, blueprint_id>> blueprints; // name -> id
    diagnostics diags;
    bool ok() const { return diags.empty(); }

    blueprint_id find(const std::string& name) const {
        for (const auto& p : blueprints) if (p.first == name) return p.second;
        return invalid_blueprint;
    }
};

// Parse + lower + register every rune in `src` into `rt`. Runtime must be
// init()'d (block size is needed to compile schedules). Registration failures
// (graph-level: cycles, rate/block inconsistencies) surface as diagnostics.
inline load_result load(runtime& rt, const std::string& src, const node_registry& reg) {
    load_result out;

    parse_result pr = parse(src);
    for (const auto& d : pr.diags) out.diags.push_back(d);
    if (!pr.ok()) return out; // do not attempt lowering on a broken parse

    lower_result lr = lower(pr.prog, reg);
    for (const auto& d : lr.diags) out.diags.push_back(d);

    for (auto& lb : lr.blueprints) {
        blueprint_builder bb(lb.name);
        bb.bp = std::move(lb.bp);
        blueprint_id id = rt.register_blueprint(bb);
        if (id == invalid_blueprint)
            out.diags.push_back({"rune '" + lb.name + "': " + rt.last_error(), 0, 0});
        else
            out.blueprints.push_back({lb.name, id});
    }
    return out;
}

// Convenience overload using the standard node vocabulary.
inline load_result load(runtime& rt, const std::string& src) {
    static const node_registry reg = standard_registry();
    return load(rt, src, reg);
}

// Load a .rune file from disk.
inline load_result load_file(runtime& rt, const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        load_result r;
        r.diags.push_back({"cannot open file: " + path, 0, 0});
        return r;
    }
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load(rt, src);
}

} // namespace xrune::lang
