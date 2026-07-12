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

#include "galdr/faust_registry.hpp"
#include "galdr/compile.hpp"
#include "offline_backend.hpp"
#include "test_util.hpp"
#include <memory>

using namespace xrune;
using namespace xrune::galdr;

static constexpr size_t SR = 48000, BS = 128;

static offline_backend* make_rt(runtime& rt) {
    auto ob = std::make_unique<offline_backend>();
    offline_backend* p = ob.get();
    rt.use_backend(std::move(ob));
    rt.init({SR, BS, 0, 2, 32, 0});
    rt.start();
    return p;
}

int main() {
    // A registry with the standard vocabulary + a JIT Faust node "fgain".
    node_registry reg = standard_registry();
    register_faust(reg, "fgain",
                   "gain = hslider(\"gain\", 0.5, 0.0, 2.0, 0.01); process = _ * gain;");

    // --- a Faust node is usable as a bare DSL word ---
    XR_RUN("faust node in Galdr (default)");
    {
        runtime rt; offline_backend* ob = make_rt(rt);
        auto r = load(rt, "rune s\n  out constant(1.0) : fgain\nend\n", reg);
        for (auto& d : r.diags) std::cerr << "  diag: " << d.format() << "\n";
        XR_CHECK(r.ok());
        rt.spawn(r.find("s"));
        ob->render(20);
        XR_CHECK_NEAR(ob->rms(0), 0.5, 1e-6); // 1.0 * faust hslider default 0.5
    }

    // --- DSL args override the Faust port default (named + positional) ---
    XR_RUN("faust port default override");
    {
        runtime rt; offline_backend* ob = make_rt(rt);
        auto r = load(rt,
            "rune named\n  out constant(1.0) : fgain(gain = 0.25)\nend\n"
            "rune positional\n  out constant(1.0) : fgain(0.1)\nend\n", reg);
        for (auto& d : r.diags) std::cerr << "  diag: " << d.format() << "\n";
        XR_CHECK(r.ok());

        voice a = rt.spawn(r.find("named"));
        ob->render(20);
        XR_CHECK_NEAR(ob->rms(0), 0.25, 1e-6);
        rt.kill(a); ob->render(1); rt.pump();

        rt.spawn(r.find("positional"));
        ob->render(20);
        XR_CHECK_NEAR(ob->rms(0), 0.1, 1e-6);
    }

    // --- a named Faust node is Idyl-addressable by port name ---
    XR_RUN("faust node addressable");
    {
        runtime rt; offline_backend* ob = make_rt(rt);
        auto r = load(rt, "rune s\n  f = fgain(0.3)\n  out constant(1.0) : f\nend\n", reg);
        for (auto& d : r.diags) std::cerr << "  diag: " << d.format() << "\n";
        XR_CHECK(r.ok());
        blueprint_id id = r.find("s");
        param_ref g = rt.resolve(id, "f", "gain");
        XR_CHECK(g.ok);                    // Idyl can reach the Faust param by name

        voice v = rt.spawn(id);
        ob->render(20);
        XR_CHECK_NEAR(ob->rms(0), 0.3, 1e-6);

        // ...and drive it at runtime.
        rt.set(v, g, 0.8);
        ob->render(2); ob->render(20);
        XR_CHECK_NEAR(ob->rms(0), 0.8, 1e-3);
    }

    XR_MAIN_REPORT();
}
