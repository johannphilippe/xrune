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

// Indexed output: `out[i] expr` places a signal on output channel i.
//
// The property that matters is placement: channel i gets exactly expr, not
// swapped with another channel and not summed into one. DC constants make that
// unambiguous -- master channel i's RMS is exactly the value routed there.

#include "xrune/api.hpp"
#include "xrune/lang/compile.hpp"
#include "xrune/serialize.hpp"
#include "xrune/audio/offline_backend.hpp"
#include "test_util.hpp"

#include <memory>
#include <string>

using namespace xrune;

static offline_backend* rt_render(runtime& rt, const std::string& src, size_t blocks = 50) {
    auto ob = std::make_unique<offline_backend>();
    offline_backend* p = ob.get();
    rt.use_backend(std::move(ob));
    rt.init({48000, 128, 0, 2, 16, 0});
    rt.start();
    lang::load_result r = lang::load(rt, src);
    if (!r.ok()) return nullptr;
    if (!rt.spawn(r.blueprints.front().second).valid()) return nullptr;
    p->render(blocks);
    return p;
}

int main() {
    // ---- placement: out[0] and out[1] land on the right, distinct channels ---
    XR_RUN("out[i] routes to output channel i");
    {
        runtime rt;
        offline_backend* p = rt_render(rt,
            "rune t()\n"
            "  out[0] constant(value = 0.2)\n"
            "  out[1] constant(value = 0.7)\n"
            "end\n");
        XR_CHECK(p != nullptr);
        XR_CHECK_NEAR(p->rms(0), 0.2, 1e-9);   // exactly what out[0] placed
        XR_CHECK_NEAR(p->rms(1), 0.7, 1e-9);   // ... not swapped, not summed
    }

    // Swap the indices: the channels must swap with them.
    XR_RUN("out[i] order is by index, not by statement order");
    {
        runtime rt;
        offline_backend* p = rt_render(rt,
            "rune t()\n"
            "  out[1] constant(value = 0.7)\n"   // written first, still channel 1
            "  out[0] constant(value = 0.2)\n"
            "end\n");
        XR_CHECK(p != nullptr);
        XR_CHECK_NEAR(p->rms(0), 0.2, 1e-9);
        XR_CHECK_NEAR(p->rms(1), 0.7, 1e-9);
    }

    // ---- a plain `out` is still `out[0..]` (back-compat) -------------------
    XR_RUN("plain out is unchanged");
    {
        runtime rt;
        // m2s fans a mono constant to two channels; both master channels equal.
        offline_backend* p = rt_render(rt,
            "rune t()\n  out constant(value = 0.3) : m2s\nend\n");
        XR_CHECK(p != nullptr);
        XR_CHECK_NEAR(p->rms(0), 0.3, 1e-9);
        XR_CHECK_NEAR(p->rms(1), 0.3, 1e-9);
    }

    // ---- the old "out must be a single node" wart is gone ------------------
    XR_RUN("out gathers channels from several nodes");
    {
        runtime rt;
        // Two independent constant nodes, placed as the two output channels.
        offline_backend* p = rt_render(rt,
            "rune t()\n  out (constant(value = 0.4) , constant(value = 0.6))\nend\n");
        XR_CHECK(p != nullptr);
        XR_CHECK_NEAR(p->rms(0), 0.4, 1e-9);
        XR_CHECK_NEAR(p->rms(1), 0.6, 1e-9);
    }

    // ---- gaps are allowed: an unassigned channel is silent -----------------
    // You do not always want to start at channel 0 -- multichannel hardware may
    // want a signal on output 5 with 0..4 untouched.
    XR_RUN("a gap is a silent channel, not an error");
    {
        runtime rt;
        offline_backend* p = rt_render(rt,
            "rune t()\n  out[1] constant(value = 0.6)\nend\n");
        XR_CHECK(p != nullptr);
        XR_CHECK_NEAR(p->rms(0), 0.0, 1e-12);   // channel 0 was never assigned
        XR_CHECK_NEAR(p->rms(1), 0.6, 1e-9);    // the signal is on channel 1
    }

    XR_RUN("a channel beyond the device is simply not heard");
    {
        runtime rt;
        // out[3] on a 2-channel device: the signal is destined for channel 3,
        // which this device does not have, so neither output carries it.
        offline_backend* p = rt_render(rt,
            "rune t()\n  out[3] constant(value = 0.5)\nend\n");
        XR_CHECK(p != nullptr);
        XR_CHECK_NEAR(p->rms(0), 0.0, 1e-12);
        XR_CHECK_NEAR(p->rms(1), 0.0, 1e-12);
    }

    // ---- errors ------------------------------------------------------------

    XR_RUN("assigning a channel twice is rejected");
    {
        runtime rt;
        rt.use_backend(std::make_unique<offline_backend>());
        rt.init({48000, 128, 0, 2, 16, 0}); rt.start();
        lang::load_result r = lang::load(rt,
            "rune t()\n  out[0] constant(value = 1)\n  out[0] constant(value = 2)\nend\n");
        XR_CHECK(!r.ok());
        XR_CHECK(r.diags.front().message.find("more than once") != std::string::npos);
    }

    // ---- indexed output survives a JSON round-trip, bit-for-bit ------------
    XR_RUN("indexed output round-trips through JSON");
    {
        runtime rt;
        rt.use_backend(std::make_unique<offline_backend>());
        rt.init({48000, 128, 0, 2, 16, 0}); rt.start();
        lang::load_result r = lang::load(rt,
            "rune t()\n"
            "  out[0] constant(value = 0.2)\n"
            "  out[1] constant(value = 0.7)\n"
            "end\n");
        XR_CHECK(r.ok());
        const graph_blueprint& bp = rt.registry[r.blueprints.front().second]->bp;

        // The output terminal now carries a per-channel source list.
        XR_CHECK(bp.output_terminals.size() == 1);
        XR_CHECK(bp.output_terminals[0].channels.size() == 2);

        graph_blueprint re;
        std::string err;
        XR_CHECK(from_json(to_json(bp), lang::standard_registry(), re, err));
        XR_CHECK(re.output_terminals.size() == 1);
        XR_CHECK(re.output_terminals[0].channels.size() == 2);
        XR_CHECK(re.output_terminals[0].channels[0].node == bp.output_terminals[0].channels[0].node);
        XR_CHECK(re.output_terminals[0].channels[1].ch  == bp.output_terminals[0].channels[1].ch);

        // Reloaded, it renders the same channel placement.
        compiled_schedule sa = compile(bp, 128), sb = compile(re, 128);
        auto ia = instantiate(sa, 48000), ib = instantiate(sb, 48000);
        for (int blk = 0; blk < 4; ++blk) { ia->process(); ib->process(); }
        for (size_t ch = 0; ch < 2; ++ch)
            for (size_t i = 0; i < 128; ++i)
                XR_CHECK(ia->output_terminal_view(0, ch)[i] == ib->output_terminal_view(0, ch)[i]);
    }

    // ---- a silent gap survives the JSON round-trip -------------------------
    XR_RUN("a silent channel round-trips through JSON");
    {
        runtime rt;
        rt.use_backend(std::make_unique<offline_backend>());
        rt.init({48000, 128, 0, 2, 16, 0}); rt.start();
        lang::load_result r = lang::load(rt, "rune t()\n  out[2] constant(value = 0.5)\nend\n");
        XR_CHECK(r.ok());
        const graph_blueprint& bp = rt.registry[r.blueprints.front().second]->bp;
        XR_CHECK(bp.output_terminals[0].channels.size() == 3);
        XR_CHECK(bp.output_terminals[0].channels[0].silent);   // 0 and 1 are gaps
        XR_CHECK(bp.output_terminals[0].channels[1].silent);
        XR_CHECK(!bp.output_terminals[0].channels[2].silent);

        graph_blueprint re;
        std::string err;
        XR_CHECK(from_json(to_json(bp), lang::standard_registry(), re, err));
        XR_CHECK(re.output_terminals[0].channels.size() == 3);
        XR_CHECK(re.output_terminals[0].channels[0].silent);
        XR_CHECK(re.output_terminals[0].channels[2].silent == false);
        XR_CHECK(re.output_terminals[0].channels[2].node == bp.output_terminals[0].channels[2].node);
    }

    // ---- input channel selection already works (verify it stays working) ---
    XR_RUN("input channels are selectable with [i]");
    {
        runtime rt;
        rt.use_backend(std::make_unique<offline_backend>());
        rt.init({48000, 128, 0, 2, 16, 0}); rt.start();
        lang::load_result r = lang::load(rt,
            "rune fx()\n"
            "  input in(channels = 2)\n"
            "  out[0] in[1]\n"          // cross the channels on the way out
            "  out[1] in[0]\n"
            "end\n");
        XR_CHECK(r.ok());
        const blueprint_info* bi = rt.describe(r.blueprints.front().second);
        XR_CHECK(bi != nullptr);
        XR_CHECK(bi->input_terminals.size() == 1);
        XR_CHECK(bi->output_terminals.size() == 1);
    }

    XR_MAIN_REPORT();
}
