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

#include "xrune/core.hpp"
#include "xrune/node/standard_nodes.hpp"
#include "xrune/blueprint.hpp"
#include "xrune/schedule.hpp"
#include "xrune/instance.hpp"
#include "xrune/node/faust/faust_jit.hpp"
#include "test_util.hpp"
#include <memory>
#include <string>

using namespace xrune;

// JIT-compile a Faust program with one control port and verify it hosts as an
// Xrune node: correct I/O + ports, correct audio, and the port drives the DSP.
int main() {
    const size_t sr = 48000, bs = 128;
    const char* dsp = "gain = hslider(\"gain\", 0.5, 0.0, 2.0, 0.01); process = _ * gain;";

    XR_RUN("faust jit hosts as a node");
    graph_blueprint bp;
    size_t c = bp.add<constant>(1.0);
    size_t f = bp.add<faust_jit>(std::string(dsp));
    bp.connect(c, 0, f, 0);
    bp.set_output(f);

    // Metadata from the Faust UI.
    node* fn = bp.node_at(f);
    XR_CHECK(fn->inputs_count() == 1);
    XR_CHECK(fn->outputs_count() == 1);
    XR_CHECK(fn->params_count() == 1);
    XR_CHECK(std::string(fn->params()[0].name) == "gain");
    XR_CHECK_NEAR(fn->param_default(0), 0.5, 1e-9);

    compiled_schedule s = compile(bp, bs);
    XR_CHECK(s.ok);
    auto v = instantiate(s, sr);
    XR_CHECK(v != nullptr);

    // constant 1.0 * gain 0.5 -> 0.5
    v->process();
    XR_CHECK_NEAR(v->output_view(0)[0], 0.5, 1e-6);

    // Drive the Faust control port through the Xrune port system.
    v->set_parameter(f, 0, 1.5);
    v->process(); // settle ramp
    v->process();
    XR_CHECK_NEAR(v->output_view(0)[0], 1.5, 1e-6);

    XR_MAIN_REPORT();
}
