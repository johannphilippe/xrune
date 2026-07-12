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
// JIT Faust node: compiles Faust source at runtime via libfaust's LLVM backend.
// The compiled factory is a shareable, compile-once object; each node object
// bound to it makes its own dsp instances (one per voice). A convenience
// constructor compiles a private factory for standalone use.
//
//   auto n = std::make_unique<xrune::faust_jit>("process = _ * hslider(\"g\",1,0,2,0.01);");

#include "faust_common.hpp"
#include <faust/dsp/llvm-dsp.h>
#include <memory>
#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>

namespace xrune {

// RAII wrapper around a compiled LLVM factory (compile once, share widely).
struct faust_factory {
    llvm_dsp_factory* f = nullptr;

    faust_factory(const std::string& code, bool is_file, const std::string& options) {
        std::vector<std::string> toks = split(options);
        bool has_double = false;
        for (const auto& t : toks) if (t == "-double") has_double = true;
        if (!has_double) toks.emplace_back("-double"); // Xrune samples are double

        std::vector<const char*> argv;
        argv.reserve(toks.size());
        for (const auto& t : toks) argv.push_back(t.c_str());

        std::string err;
        f = is_file ? createDSPFactoryFromFile(code, static_cast<int>(argv.size()), argv.data(), "", err)
                    : createDSPFactoryFromString("xrune", code, static_cast<int>(argv.size()), argv.data(), "", err);
        if (!f) throw std::runtime_error("faust compile failed: " + err);
    }
    ~faust_factory() { if (f) deleteDSPFactory(f); }
    faust_factory(const faust_factory&) = delete;
    faust_factory& operator=(const faust_factory&) = delete;

    ::dsp* instance() const { return f->createDSPInstance(); }

    static std::vector<std::string> split(const std::string& s) {
        std::vector<std::string> out; std::istringstream ss(s); std::string t;
        while (ss >> t) out.push_back(t);
        return out;
    }
};

struct faust_jit : faust_node_base {
    std::shared_ptr<faust_factory> factory;

    // Share a pre-compiled factory (registry / multiple nodes).
    explicit faust_jit(std::shared_ptr<faust_factory> fac) : factory(std::move(fac)) { init_meta(); }

    // Compile a private factory (standalone). is_file: code is a .dsp path.
    explicit faust_jit(const std::string& code, bool is_file = false, const std::string& opts = "-double")
        : factory(std::make_shared<faust_factory>(code, is_file, opts)) { init_meta(); }

    ::dsp* make_dsp() const override { return factory->instance(); }
    void destroy_dsp(::dsp* d) const override { delete d; }
};

} // namespace xrune
