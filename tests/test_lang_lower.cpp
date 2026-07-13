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

#include "xrune/lang/compile.hpp"
#include "xrune/audio/offline_backend.hpp"
#include "test_util.hpp"
#include <memory>

using namespace xrune;
using namespace xrune::lang;

static constexpr double SINE_RMS = 0.70710678118654752440;
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
    // --- end to end: a rune compiles to correct audio ---
    XR_RUN("synth end-to-end");
    {
        runtime rt; offline_backend* ob = make_rt(rt);
        auto r = load(rt,
            "rune synth\n"
            "  osc = sine(freq = 440)\n"
            "  amp = gain(0.25)\n"
            "  osc : amp\n"
            "  out amp\n"
            "end\n");
        for (auto& d : r.diags) std::cerr << "  diag: " << d.format() << "\n";
        XR_CHECK(r.ok());
        blueprint_id id = r.find("synth");
        XR_CHECK(id != invalid_blueprint);

        // Idyl addressability: the named nodes are in the introspection.
        const blueprint_info* info = rt.describe(id);
        XR_CHECK(info != nullptr);
        XR_CHECK(rt.resolve(id, "osc", "freq").ok);
        XR_CHECK(rt.resolve(id, "amp", "gain").ok);

        voice v = rt.spawn(id);
        XR_CHECK(v.valid());
        ob->render(200);
        XR_CHECK_NEAR(ob->rms(0), SINE_RMS * 0.25, 0.005);
    }

    // --- ':>' materializes a mixer and sums correctly ---
    XR_RUN("merge sums via materialized mixer");
    {
        runtime rt; offline_backend* ob = make_rt(rt);
        auto r = load(rt,
            "rune s3\n"
            "  out (constant(0.1) , constant(0.2) , constant(0.3)) :> gain(1.0)\n"
            "end\n");
        for (auto& d : r.diags) std::cerr << "  diag: " << d.format() << "\n";
        XR_CHECK(r.ok());
        rt.spawn(r.find("s3"));
        ob->render(20);
        XR_CHECK_NEAR(ob->rms(0), 0.6, 1e-6); // 0.1+0.2+0.3 DC
    }

    // --- audio-rate modulation via ~> ---
    XR_RUN("modulation");
    {
        runtime rt; offline_backend* ob = make_rt(rt);
        auto r = load(rt,
            "rune m\n"
            "  dc  = constant(1.0)\n"
            "  amp = gain(1.0)\n"
            "  lfo = sine(freq = 2000)\n"
            "  dc : amp\n"
            "  lfo ~> amp.gain\n"
            "  out amp\n"
            "end\n");
        for (auto& d : r.diags) std::cerr << "  diag: " << d.format() << "\n";
        XR_CHECK(r.ok());
        rt.spawn(r.find("m"));
        ob->render(200);
        XR_CHECK_NEAR(ob->rms(0), SINE_RMS, 0.01); // 1.0 * sine -> sine RMS
    }

    // --- sigil expansion + params + arithmetic ---
    XR_RUN("sigil");
    {
        runtime rt; offline_backend* ob = make_rt(rt);
        auto r = load(rt,
            "sigil pair(f)\n"
            "  sine(freq = f) , sine(freq = f * 2)\n"
            "end\n"
            "rune two\n"
            "  out pair(220) :> gain(0.25)\n"
            "end\n");
        for (auto& d : r.diags) std::cerr << "  diag: " << d.format() << "\n";
        XR_CHECK(r.ok());
        rt.spawn(r.find("two"));
        ob->render(400);
        // two incommensurate sines summed: RMS = sqrt(rms^2 + rms^2) = 1.0, * 0.25
        XR_CHECK_NEAR(ob->rms(0), 0.25, 0.01);
    }

    // --- cross-rune bus routing built from the DSL ---
    XR_RUN("bus + routing");
    {
        runtime rt; offline_backend* ob = make_rt(rt);
        auto r = load(rt,
            "rune voice\n"
            "  out sine(440) : gain(0.25)\n"
            "end\n"
            "rune bus\n"
            "  input in(channels = 1)\n"
            "  out in : gain(0.5)\n"
            "end\n");
        for (auto& d : r.diags) std::cerr << "  diag: " << d.format() << "\n";
        XR_CHECK(r.ok());

        voice b = rt.spawn(r.find("bus"));
        spawn_options into; into.into = b;
        rt.spawn(r.find("voice"), into);
        ob->render(200);
        XR_CHECK_NEAR(ob->rms(0), SINE_RMS * 0.25 * 0.5, 0.005);
    }

    // --- sigil-internal nodes get stable, per-call-site addressable names ---
    XR_RUN("name prefixing (Idyl addressability)");
    {
        runtime rt; make_rt(rt);
        auto r = load(rt,
            "sigil chain(f, g)\n"
            "  osc = sine(freq = f)\n"
            "  amp = gain(g)\n"
            "  osc : amp\n"
            "end\n"
            "rune poly\n"
            "  v1 = chain(220, 0.2)\n"
            "  v2 = chain(330, 0.3)\n"
            "  out (v1 , v2) :> gain(0.5)\n"
            "end\n");
        for (auto& d : r.diags) std::cerr << "  diag: " << d.format() << "\n";
        XR_CHECK(r.ok());
        blueprint_id id = r.find("poly");
        XR_CHECK(id != invalid_blueprint);

        // Each call site's internal nodes are addressable by dotted path.
        param_ref v1f = rt.resolve(id, "v1.osc", "freq");
        param_ref v2f = rt.resolve(id, "v2.osc", "freq");
        XR_CHECK(v1f.ok);
        XR_CHECK(v2f.ok);
        XR_CHECK(rt.resolve(id, "v1.amp", "gain").ok);
        XR_CHECK(rt.resolve(id, "v2.amp", "gain").ok);
        XR_CHECK(v1f.node != v2f.node); // distinct per call site

        // ...and they appear in the introspection Idyl reads.
        const blueprint_info* info = rt.describe(id);
        bool has_v1osc = false, has_v2amp = false;
        for (const auto& n : info->nodes) {
            if (n.name == "v1.osc") has_v1osc = true;
            if (n.name == "v2.amp") has_v2amp = true;
        }
        XR_CHECK(has_v1osc);
        XR_CHECK(has_v2amp);

        // Inline (unbound) sigil calls stay anonymous/unprefixed (no regression).
        auto a = load(rt, "rune anon\n  out sine(440) : gain(0.5)\nend\n");
        XR_CHECK(a.ok());
    }

    // --- the shipped example file loads and makes sound ---
#ifdef XRUNE_SOURCE_DIR
    XR_RUN("examples/drone.rune loads");
    {
        runtime rt; offline_backend* ob = make_rt(rt);
        auto r = lang::load_file(rt, std::string(XRUNE_SOURCE_DIR) + "/examples/drone.rune");
        for (auto& d : r.diags) std::cerr << "  diag: " << d.format() << "\n";
        XR_CHECK(r.ok());
        blueprint_id id = r.find("drone");
        XR_CHECK(id != invalid_blueprint);
        rt.spawn(id);
        ob->render(50);
        XR_CHECK(ob->peak(0) > 0.0); // produced sound
    }
#endif

    // --- errors carry through with messages ---
    XR_RUN("errors");
    {
        runtime rt; make_rt(rt);
        XR_CHECK(!load(rt, "rune a\n  out nope()\nend\n").ok());        // unknown node
        XR_CHECK(!load(rt, "rune a\n  out sine(440) : add\nend\n").ok()); // ':' arity 1 vs 2
        XR_CHECK(!load(rt, "rune a\n  x = gain(1.0)\nend\n").ok());       // no 'out'
        XR_CHECK(!load(rt, "rune a\n  out 0.5 : gain(1)\nend\n").ok());   // kind: number in wiring
    }

    XR_MAIN_REPORT();
}
