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
// Register Faust programs as Galdr/registry node types, so a .dsp becomes a DSL
// word usable like any built-in node (`myfaust(param = value)`, modulation into
// its ports, per-voice instances). Guarded by the Faust CMake options; without
// them this header adds nothing.
//
//   node_registry reg = galdr::standard_registry();
//   galdr::register_faust(reg, "reverb", "reverb.dsp", /*is_file=*/true);   // JIT
//   galdr::register_faust_static<mydsp>(reg, "synth");                      // static
//   galdr::load(rt, source, reg);

#include "node_registry.hpp"

#if defined(XRUNE_WITH_FAUST_LLVM)
#include "../node/faust/faust_jit.hpp"
#endif
#if defined(XRUNE_WITH_FAUST)
#include "../node/faust/faust_static.hpp"
#endif

#include <string>
#include <memory>

namespace xrune::galdr {

// Apply DSL call arguments as Faust port-default overrides (positional by port
// index, named by port name).
inline void apply_faust_args(faust_node_base& n, const node_args& args) {
    for (size_t i = 0; i < args.positional.size(); ++i)
        n.set_port_default_index(i, args.positional[i].number);
    for (const auto& kv : args.named)
        n.set_port_default(kv.first, kv.second.number);
}

#if defined(XRUNE_WITH_FAUST_LLVM)
// JIT: compile the Faust program once, share the factory across all uses.
inline void register_faust(node_registry& reg, const std::string& name,
                           const std::string& code, bool is_file = false,
                           const std::string& opts = "-double") {
    auto factory = std::make_shared<faust_factory>(code, is_file, opts);
    reg.add(name, [factory](const node_args& args) {
        auto n = std::make_unique<faust_jit>(factory);
        apply_faust_args(*n, args);
        return n;
    });
}
inline void register_faust_file(node_registry& reg, const std::string& name,
                                const std::string& path, const std::string& opts = "-double") {
    register_faust(reg, name, path, /*is_file=*/true, opts);
}
#endif

#if defined(XRUNE_WITH_FAUST)
// Static: wrap a Faust-generated dsp class as a DSL word.
template <class Dsp>
inline void register_faust_static(node_registry& reg, const std::string& name) {
    reg.add(name, [](const node_args& args) {
        auto n = std::make_unique<faust_static<Dsp>>();
        apply_faust_args(*n, args);
        return n;
    });
}
#endif

} // namespace xrune::galdr
