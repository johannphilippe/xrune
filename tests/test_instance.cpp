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

#include "core.hpp"
#include "standard_nodes.hpp"
#include "blueprint.hpp"
#include "schedule.hpp"
#include "instance.hpp"
#include "test_util.hpp"
#include <memory>
#include <cmath>
#include <cstdint>

using namespace xrune;

// Are two instances' current output-channel-0 blocks equal?
static bool block_equal(const graph_instance& a, const graph_instance& b, double eps = 1e-12) {
    audio_buffer_view va = a.output_view(0);
    audio_buffer_view vb = b.output_view(0);
    if (va.size != vb.size) return false;
    for (size_t i = 0; i < va.size; ++i)
        if (std::fabs(va[i] - vb[i]) > eps) return false;
    return true;
}

int main() {
    const size_t sr = 48000;
    const size_t bs = 64;

    // Shared blueprint + schedule; three instances of it.
    graph_blueprint bp;
    size_t osc = bp.add<oscillator>(440.0);
    size_t gn = bp.add<gain>(0.5);
    bp.connect(osc, 0, gn, 0);
    bp.set_output(gn);

    compiled_schedule sched = compile(bp, bs);
    XR_CHECK(sched.ok);
    XR_CHECK(sched.topo_order.size() == 2);

    auto a = instantiate(sched, sr);
    auto b = instantiate(sched, sr);
    auto c = instantiate(sched, sr);
    XR_CHECK(a && b && c);

    // Distinct arena regions => genuinely independent state.
    XR_CHECK(a->buffer_pool != b->buffer_pool);
    XR_CHECK(b->buffer_pool != c->buffer_pool);

    // --- identical params => identical, deterministic output across instances ---
    XR_RUN("instances deterministic before divergence");
    for (int blk = 0; blk < 4; ++blk) { a->process(); b->process(); c->process(); }
    XR_CHECK(block_equal(*a, *b));
    XR_CHECK(block_equal(*a, *c));

    // --- detune only b; a and c must be unaffected, b must diverge ---
    XR_RUN("per-instance state isolation");
    b->set_parameter(osc, 0, 220.0);
    for (int blk = 0; blk < 4; ++blk) { a->process(); b->process(); c->process(); }

    XR_CHECK(block_equal(*a, *c));     // changing b did not touch a or c
    XR_CHECK(!block_equal(*a, *b));    // b now differs -> state is per-instance

    // --- a's signal is still a valid 440 Hz sine at gain 0.5 ---
    XR_RUN("untouched instance still correct");
    {
        double sumsq = 0.0; size_t count = 0;
        for (int blk = 0; blk < 200; ++blk) {
            a->process();
            audio_buffer_view v = a->output_view(0);
            for (size_t i = 0; i < v.size; ++i) { sumsq += v[i] * v[i]; ++count; }
        }
        double rms = std::sqrt(sumsq / static_cast<double>(count));
        XR_CHECK_NEAR(rms, 0.70710678118654752440 * 0.5, 0.005);
    }

    // --- audio buffers are SIMD-aligned (per-channel, with a pow2 block) ---
    XR_RUN("buffers are 64-byte aligned");
    {
        graph_blueprint b;
        size_t osc = b.add<oscillator>(440.0);
        size_t gn  = b.add<gain>(0.5);
        size_t m2s = b.add<mono_to_stereo>();
        b.connect(osc, 0, gn, 0);
        b.connect(gn, 0, m2s, 0);
        b.set_output(m2s);
        compiled_schedule sd = compile(b, 128); // power-of-two block
        auto vi = instantiate(sd, sr);

        for (size_t node : {osc, gn, m2s})
            for (size_t ch = 0; ch < sd.bp->nodes[node]->outputs_count(); ++ch) {
                auto* p = vi->node_output_view(node, ch).data;
                XR_CHECK(reinterpret_cast<uintptr_t>(p) % simd_align == 0);
            }
    }

    XR_MAIN_REPORT();
}
