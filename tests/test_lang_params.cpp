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

// Rune parameters promoted to *instance* parameters:
//   - a host sees them under the names the user wrote (`describe`)
//   - they can be set at run time by name (`set_param`)
//   - they can be set AT SPAWN, with no ramp (`spawn_options::params`)
//   - a parameter that was folded into an expression is reported as `partial`

#include "xrune/api.hpp"
#include "xrune/lang/compile.hpp"
#include "xrune/audio/offline_backend.hpp"
#include "test_util.hpp"

#include <memory>
#include <string>

using namespace xrune;

static constexpr size_t SR = 48000, BS = 128;

// `f` is used directly (a.freq) AND inside an expression (b.freq = f * det),
// so it is bound but *partial*. `amp` is a clean direct use. `det` is only ever
// used inside an expression, so it binds to nothing.
static const char* SRC =
    "rune tone(f = 220, amp = 0.3, det = 1.01)\n"
    "  a = sine(freq = f)\n"
    "  b = sine(freq = f * det)\n"
    "  out (a , b) :> gain(amp)\n"
    "end\n";

struct rig {
    runtime rt;
    offline_backend* ob = nullptr;
    blueprint_id id = invalid_blueprint;

    explicit rig(const char* src = SRC) {
        auto o = std::make_unique<offline_backend>();
        ob = o.get();
        rt.use_backend(std::move(o));
        rt.init({SR, BS, 0, 2, 16, 0});
        rt.start();
        lang::load_result r = lang::load(rt, src);
        if (r.ok() && !r.blueprints.empty()) id = r.blueprints.front().second;
    }
};

int main() {
    // ---- the host can discover the rune's own parameters -------------------
    XR_RUN("describe() exposes rune parameters by their source names");
    {
        rig g;
        XR_CHECK(g.id != invalid_blueprint);
        const blueprint_info* bi = g.rt.describe(g.id);
        XR_CHECK(bi != nullptr);
        XR_CHECK(bi->params.size() == 3);

        XR_CHECK(bi->params[0].name == "f");
        XR_CHECK_NEAR(bi->params[0].default_value, 220.0, 1e-12);
        XR_CHECK(bi->params[0].targets == 1);        // bound to a.freq
        XR_CHECK(bi->params[0].partial);             // ... but also folded into f*det

        XR_CHECK(bi->params[1].name == "amp");
        XR_CHECK_NEAR(bi->params[1].default_value, 0.3, 1e-12);
        XR_CHECK(bi->params[1].targets == 1);
        XR_CHECK(!bi->params[1].partial);            // a clean direct use

        XR_CHECK(bi->params[2].name == "det");
        XR_CHECK(bi->params[2].targets == 0);        // only ever used in f*det
        XR_CHECK(bi->params[2].partial);
    }

    // ---- set_param drives the port the parameter was passed to -------------
    XR_RUN("set_param() by rune-parameter name");
    {
        // A DC rune: the output IS the parameter, so the assertion is exact.
        // (The 220/222.2 pair in SRC beats at 2.2 Hz, so its RMS over any finite
        // window depends on where in the beat cycle you measure -- useless here.)
        rig g("rune t(amp = 0.3)\n  out constant(value = 1) : gain(amp)\nend\n");
        voice v = g.rt.spawn(g.id);
        XR_CHECK(v.valid());

        g.ob->render(50);
        XR_CHECK_NEAR(g.ob->rms(0), 0.3, 1e-9);

        XR_CHECK(g.rt.set_param(v, "amp", 0.6));
        g.ob->render(2);                  // settle the ramp
        g.ob->render(50);
        XR_CHECK_NEAR(g.ob->rms(0), 0.6, 1e-9);
    }

    // `osc * amp` is the natural way to write a level control. Signal arithmetic
    // builds the gain node itself, bypassing the node-call binder -- so this has
    // to be bound explicitly, or the most idiomatic form would be unaddressable.
    XR_RUN("a parameter used as `sig * param` is bound too");
    {
        rig g("rune t(amp = 0.25)\n  out constant(value = 1) * amp\nend\n");
        const blueprint_info* bi = g.rt.describe(g.id);
        XR_CHECK(bi->params.size() == 1);
        XR_CHECK(bi->params[0].name == "amp");
        XR_CHECK(bi->params[0].targets == 1);      // bound to the gain it created
        XR_CHECK(!bi->params[0].partial);

        voice v = g.rt.spawn(g.id);
        g.ob->render(20);
        XR_CHECK_NEAR(g.ob->rms(0), 0.25, 1e-9);

        XR_CHECK(g.rt.set_param(v, "amp", 0.75));
        g.ob->render(2);
        g.ob->render(20);
        XR_CHECK_NEAR(g.ob->rms(0), 0.75, 1e-9);
    }

    XR_RUN("set_param() rejects unknown / unbound parameters");
    {
        rig g;
        voice v = g.rt.spawn(g.id);

        XR_CHECK(!g.rt.set_param(v, "nope", 1.0));
        XR_CHECK(g.rt.last_error().find("unknown rune parameter") != std::string::npos);

        // `det` was folded into an expression: it drives no port. Say so, rather
        // than accept the call and silently do nothing.
        XR_CHECK(!g.rt.set_param(v, "det", 1.05));
        XR_CHECK(g.rt.last_error().find("drives no port") != std::string::npos);
    }

    // ---- spawn-time overrides ---------------------------------------------
    XR_RUN("spawn_options::params sets the value for that voice");
    {
        rig g;
        spawn_options o;
        o.params = {{"amp", 0.6}};
        voice v = g.rt.spawn(g.id, o);
        XR_CHECK(v.valid());

        rig h;                                   // same rune, default amp = 0.3
        voice w = h.rt.spawn(h.id);

        g.ob->render(200);
        h.ob->render(200);
        XR_CHECK_NEAR(g.ob->rms(0), h.ob->rms(0) * 2.0, 0.01);   // 0.6 vs 0.3
    }

    XR_RUN("spawn_options::params rejects unknown / unbound parameters");
    {
        rig g;
        spawn_options bad;
        bad.params = {{"nope", 1.0}};
        XR_CHECK(!g.rt.spawn(g.id, bad).valid());
        XR_CHECK(g.rt.last_error().find("unknown rune parameter") != std::string::npos);

        spawn_options folded;
        folded.params = {{"det", 1.05}};
        XR_CHECK(!g.rt.spawn(g.id, folded).valid());
        XR_CHECK(g.rt.last_error().find("drives no port") != std::string::npos);
    }

    // ---- THE POINT: a spawn-time value must not GLIDE ----------------------
    // Setting a port after spawn ramps from the compiled default across the
    // first block (right for a live change, a chirp on a note-on). A spawn-time
    // value must be there from sample zero.
    XR_RUN("spawn-time value is exact from the first sample (no ramp)");
    {
        // A DC rune, so the output IS the parameter value — any ramp shows up
        // directly in the first block's peak/rms.
        const char* dc = "rune d(level = 0.1)\n  out constant(value = level)\nend\n";

        // (a) spawn with an override: block 0 must already be at 0.9
        rig g(dc);
        spawn_options o;
        o.params = {{"level", 0.9}};
        voice v = g.rt.spawn(g.id, o);
        XR_CHECK(v.valid());
        g.ob->render(1);                          // ONE block
        XR_CHECK_NEAR(g.ob->rms(0), 0.9, 1e-9);   // exact: no glide from 0.1
        XR_CHECK_NEAR(g.ob->peak(0), 0.9, 1e-9);

        // (b) spawn then set: block 0 ramps 0.1 -> 0.9, so its mean is BELOW 0.9.
        // This is the artefact spawn-time overrides exist to avoid.
        rig h(dc);
        voice w = h.rt.spawn(h.id);
        XR_CHECK(h.rt.set_param(w, "level", 0.9));
        h.ob->render(1);
        XR_CHECK(h.ob->rms(0) < 0.9 - 0.01);      // it glided
        XR_CHECK(h.ob->rms(0) > 0.1);             // ... but it did move

        // and after the ramp completes, both agree
        h.ob->render(50);
        XR_CHECK_NEAR(h.ob->rms(0), 0.9, 1e-9);
    }

    XR_RUN("each voice keeps its own parameter value");
    {
        const char* dc = "rune d(level = 0.1)\n  out constant(value = level)\nend\n";
        rig g(dc);

        spawn_options a; a.params = {{"level", 0.2}};
        spawn_options b; b.params = {{"level", 0.5}};
        XR_CHECK(g.rt.spawn(g.id, a).valid());
        XR_CHECK(g.rt.spawn(g.id, b).valid());

        g.ob->render(4);
        // Two voices summed to master: 0.2 + 0.5 = 0.7. If they shared state the
        // sum would be 0.4 or 1.0 instead.
        XR_CHECK_NEAR(g.ob->rms(0), 0.7, 1e-9);
    }

    XR_MAIN_REPORT();
}
