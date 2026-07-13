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
#include "xrune/node/faust/faust_static.hpp"  // brings faust dsp/UI headers
#include "faustgain.hpp"                // generated at build time (class faustgain)
#include "test_util.hpp"
#include <memory>
#include <string>

using namespace xrune;

// Host a Faust-generated dsp class (compiled ahead of time) as an Xrune node.
int main() {
    const size_t sr = 48000, bs = 128;

    XR_RUN("faust static hosts a generated dsp");
    graph_blueprint bp;
    size_t c = bp.add<constant>(1.0);
    size_t f = bp.add<faust_static<faustgain>>();
    bp.connect(c, 0, f, 0);
    bp.set_output(f);

    node* fn = bp.node_at(f);
    XR_CHECK(fn->inputs_count() == 1);
    XR_CHECK(fn->outputs_count() == 1);
    XR_CHECK(fn->params_count() == 1);
    XR_CHECK(std::string(fn->params()[0].name) == "gain");
    XR_CHECK_NEAR(fn->param_default(0), 0.5, 1e-9);

    compiled_schedule s = compile(bp, bs);
    XR_CHECK(s.ok);
    auto v = instantiate(s, sr);
    v->process();
    XR_CHECK_NEAR(v->output_view(0)[0], 0.5, 1e-6);

    v->set_parameter(f, 0, 1.5);
    v->process();
    v->process();
    XR_CHECK_NEAR(v->output_view(0)[0], 1.5, 1e-6);

    XR_MAIN_REPORT();
}
