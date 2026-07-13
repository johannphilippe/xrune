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

// Signal arithmetic, constant coercion, and the `_` / `!` wires.
// These render real audio and check the numbers — compiling is not enough.

#include "xrune/api.hpp"
#include "xrune/lang/compile.hpp"
#include "xrune/audio/offline_backend.hpp"
#include "test_util.hpp"

#include <cmath>
#include <memory>
#include <string>

using namespace xrune;

static constexpr double SINE_RMS = 0.70710678118654752440;

// Compile `src`, spawn its single rune, render, and return the master RMS/peak.
struct rendered {
    bool ok = false;
    double rms = 0.0;
    double peak = 0.0;
    std::string err;
};

static rendered run(const std::string& src, size_t blocks = 200) {
    rendered out;
    runtime rt;
    auto ob = std::make_unique<offline_backend>();
    offline_backend* p = ob.get();
    rt.use_backend(std::move(ob));
    rt.init({48000, 128, 0, 2, 16, 0});
    rt.start();

    lang::load_result r = lang::load(rt, src);
    if (!r.ok()) {
        out.err = r.diags.front().message;
        return out;
    }
    if (r.blueprints.empty()) { out.err = "no runes"; return out; }
    voice v = rt.spawn(r.blueprints.front().second);
    if (!v.valid()) { out.err = "spawn failed"; return out; }

    p->render(blocks);
    out.ok = true;
    out.rms = p->rms(0);
    out.peak = p->peak(0);
    return out;
}

int main() {
    // ---- §1: a number in signal position becomes a constant (DC) signal ----
    XR_RUN("number coerces to a constant signal");
    {
        rendered r = run("rune t()\n  out 0.25\nend\n");
        XR_CHECK(r.ok);
        XR_CHECK_NEAR(r.rms, 0.25, 1e-9);      // DC: rms == the value
        XR_CHECK_NEAR(r.peak, 0.25, 1e-9);

        // ... and it still composes with the algebra
        rendered g = run("rune t()\n  out 0.5 : gain(0.5)\nend\n");
        XR_CHECK(g.ok);
        XR_CHECK_NEAR(g.rms, 0.25, 1e-9);
    }

    // ---- §2: signal * number -> gain -------------------------------------
    XR_RUN("sig * num scales");
    {
        rendered r = run("rune t()\n  out sine(440) * 0.5\nend\n");
        XR_CHECK(r.ok);
        XR_CHECK_NEAR(r.rms, SINE_RMS * 0.5, 0.005);

        rendered c = run("rune t()\n  out 0.5 * sine(440)\nend\n");   // commutative
        XR_CHECK(c.ok);
        XR_CHECK_NEAR(c.rms, SINE_RMS * 0.5, 0.005);
    }

    XR_RUN("sig / num folds the reciprocal");
    {
        rendered r = run("rune t()\n  out 1 / 4\nend\n");             // pure compile-time
        XR_CHECK(r.ok);
        XR_CHECK_NEAR(r.rms, 0.25, 1e-9);

        rendered s = run("rune t()\n  out 0.5 : gain(1) / 2\nend\n"); // signal / number
        XR_CHECK(s.ok);
        XR_CHECK_NEAR(s.rms, 0.25, 1e-9);
    }

    // ---- §2: signal + signal, element-wise -------------------------------
    XR_RUN("sig + sig adds");
    {
        rendered r = run("rune t()\n  out 0.25 + 0.5 : gain(1)\nend\n");
        XR_CHECK(r.ok);
        XR_CHECK_NEAR(r.rms, 0.75, 1e-9);

        // a signal minus itself is silence — proves `-` really subtracts
        rendered z = run("rune t()\n  s = sine(440)\n  out s - s\nend\n");
        XR_CHECK(z.ok);
        XR_CHECK_NEAR(z.rms, 0.0, 1e-9);
        XR_CHECK_NEAR(z.peak, 0.0, 1e-9);
    }

    XR_RUN("num - sig inverts around the constant");
    {
        rendered r = run("rune t()\n  out 1 - 0.25\nend\n");          // compile-time
        XR_CHECK(r.ok);
        XR_CHECK_NEAR(r.rms, 0.75, 1e-9);

        // 1 - x, where x is a real signal (0.25 through a gain so it stays one)
        rendered s = run("rune t()\n  out 1 - (0.25 : gain(1))\nend\n");
        XR_CHECK(s.ok);
        XR_CHECK_NEAR(s.rms, 0.75, 1e-9);
    }

    XR_RUN("unary minus inverts a signal");
    {
        rendered r = run("rune t()\n  out -(0.5 : gain(1))\nend\n");
        XR_CHECK(r.ok);
        XR_CHECK_NEAR(r.peak, 0.5, 1e-9);     // peak() is |x|
        // adding it back to the original must cancel
        rendered z = run("rune t()\n  s = 0.5 : gain(1)\n  out s + (-s)\nend\n");
        XR_CHECK(z.ok);
        XR_CHECK_NEAR(z.rms, 0.0, 1e-9);
    }

    // ---- §2: division, including the /0 policy ----------------------------
    XR_RUN("sig / sig divides; /0 yields 0, not NaN");
    {
        rendered r = run("rune t()\n  out (1 : gain(1)) / (4 : gain(1))\nend\n");
        XR_CHECK(r.ok);
        XR_CHECK_NEAR(r.rms, 0.25, 1e-9);

        // divide by a zero signal: must be 0, and must NOT be NaN — one NaN
        // would poison every downstream sample of the voice forever.
        rendered z = run("rune t()\n  out (1 : gain(1)) / (0 : gain(1))\nend\n");
        XR_CHECK(z.ok);
        XR_CHECK(!std::isnan(z.rms));
        XR_CHECK(!std::isnan(z.peak));
        XR_CHECK_NEAR(z.rms, 0.0, 1e-12);
    }

    // ---- §2: broadcast + arity -------------------------------------------
    XR_RUN("mono operand broadcasts across channels");
    {
        // (a,b) * 0.5 scales both channels
        rendered r = run("rune t()\n  out (0.5 , 0.25) * 0.5 :> _\nend\n");
        XR_CHECK(r.ok);
        XR_CHECK_NEAR(r.rms, 0.25 + 0.125, 1e-9);   // summed by :> _
    }

    XR_RUN("mismatched channel counts are rejected");
    {
        rendered r = run("rune t()\n  out (0.1,0.2,0.3) + (0.1,0.2)\nend\n");
        XR_CHECK(!r.ok);
        XR_CHECK(r.err.find("cannot combine 3 and 2") != std::string::npos);
    }

    XR_RUN("'%' stays compile-time only");
    {
        rendered r = run("rune t()\n  out (1 : gain(1)) % 2\nend\n");
        XR_CHECK(!r.ok);
        XR_CHECK(r.err.find("compile-time only") != std::string::npos);
    }

    // ---- §3: the `_` wire -------------------------------------------------
    XR_RUN("`_` is the identity wire");
    {
        rendered r = run("rune t()\n  out 0.4 : _ : gain(0.5)\nend\n");
        XR_CHECK(r.ok);
        XR_CHECK_NEAR(r.rms, 0.2, 1e-9);      // identity changed nothing
    }

    XR_RUN("`:> _` sums N channels to one");
    {
        rendered r = run("rune t()\n  out (0.1 , 0.2 , 0.3 , 0.4) :> _\nend\n");
        XR_CHECK(r.ok);
        XR_CHECK_NEAR(r.rms, 1.0, 1e-9);      // 0.1+0.2+0.3+0.4
    }

    // ---- §3: the `!` cut --------------------------------------------------
    XR_RUN("`!` discards a channel");
    {
        // keep channel 0, throw channel 1 away
        rendered r = run("rune t()\n  out (0.3 , 0.9) : (_ , !)\nend\n");
        XR_CHECK(r.ok);
        XR_CHECK_NEAR(r.rms, 0.3, 1e-9);      // 0.9 is gone, not summed in

        // and the other way round
        rendered s = run("rune t()\n  out (0.3 , 0.9) : (! , _)\nend\n");
        XR_CHECK(s.ok);
        XR_CHECK_NEAR(s.rms, 0.9, 1e-9);
    }

    // ---- the motivating case: readable modulation -------------------------
    XR_RUN("the vibrato idiom now reads like maths");
    {
        // freq = 330 + lfo*4, driving a port at audio rate
        rendered r = run(
            "rune t()\n"
            "  lfo = sine(0.5)\n"
            "  osc = sine(330)\n"
            "  (330 + lfo * 4) ~> osc.freq\n"
            "  out osc * 0.5\n"
            "end\n");
        XR_CHECK(r.ok);
        XR_CHECK_NEAR(r.rms, SINE_RMS * 0.5, 0.02);   // still a sine, still scaled
    }

    XR_MAIN_REPORT();
}
