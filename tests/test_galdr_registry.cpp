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

#include "galdr/node_registry.hpp"
#include "test_util.hpp"
#include <memory>

using namespace xrune;
using namespace xrune::galdr;

static node_args named(const std::string& n, double v) {
    node_args a; a.named.push_back({n, arg_value(v)}); return a;
}
static node_args positional(double v) {
    node_args a; a.positional.push_back(arg_value(v)); return a;
}

int main() {
    node_registry r = standard_registry();

    XR_RUN("registry membership");
    XR_CHECK(r.has("sine"));
    XR_CHECK(r.has("gain"));
    XR_CHECK(r.has("stft"));
    XR_CHECK(!r.has("nope"));
    XR_CHECK(r.make("nope", {}) == nullptr);

    // --- port defaults via named / positional / fallback ---
    XR_RUN("port defaults");
    {
        auto osc = r.make("sine", named("freq", 220.0));
        XR_CHECK(osc != nullptr);
        XR_CHECK(osc->inputs_count() == 0);
        XR_CHECK(osc->outputs_count() == 1);
        XR_CHECK(osc->params_count() == 1);
        XR_CHECK_NEAR(osc->param_default(0), 220.0, 1e-12); // named freq

        auto osc_def = r.make("sine", {});
        XR_CHECK_NEAR(osc_def->param_default(0), 440.0, 1e-12); // registry default

        auto g = r.make("gain", positional(0.5));
        XR_CHECK_NEAR(g->param_default(0), 0.5, 1e-12); // positional

        // named overrides positional
        node_args mixed;
        mixed.positional.push_back(arg_value(100.0));
        mixed.named.push_back({"freq", arg_value(200.0)});
        auto osc2 = r.make("sine", mixed);
        XR_CHECK_NEAR(osc2->param_default(0), 200.0, 1e-12);
    }

    // --- structural args configure I/O ---
    XR_RUN("structural args");
    {
        auto m = r.make("mix", named("inputs", 3.0));
        XR_CHECK(m->inputs_count() == 3);
        XR_CHECK(m->outputs_count() == 1);

        node_args io;
        io.named.push_back({"inputs", arg_value(4.0)});
        io.named.push_back({"outputs", arg_value(2.0)});
        auto ad = r.make("adapt", io);
        XR_CHECK(ad->inputs_count() == 4);
        XR_CHECK(ad->outputs_count() == 2);

        auto b = r.make("bus", named("channels", 2.0));
        XR_CHECK(b->inputs_count() == 0);
        XR_CHECK(b->outputs_count() == 2);

        XR_CHECK(r.find("adapt")->structural.size() == 2);
        XR_CHECK(r.find("gain")->structural.empty());
    }

    // --- port introspection (what the compiler reads for modulation targets) ---
    XR_RUN("port introspection");
    {
        auto p = r.make("pan", {});
        XR_CHECK(p->inputs_count() == 1);
        XR_CHECK(p->outputs_count() == 2);
        XR_CHECK(p->params_count() == 1);
        XR_CHECK(std::string(p->params()[0].name) == "pan");

        auto stft = r.make("stft", named("size", 256.0));
        XR_CHECK(stft->inputs_count() == 1);
        XR_CHECK(stft->outputs_count() == 1);
    }

    XR_MAIN_REPORT();
}
