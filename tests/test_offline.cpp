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
#include "xrune/engine.hpp"
#include "xrune/audio/offline_backend.hpp"
#include "test_util.hpp"
#include <memory>

using namespace xrune;

// RMS of a full-amplitude sine is 1/sqrt(2); scaled by the gain.
static constexpr double SINE_RMS = 0.70710678118654752440;

// Blueprint: oscillator(freq) -> gain(g), output = gain (1 channel).
static graph_blueprint make_voice(double freq, double g) {
    graph_blueprint bp;
    size_t osc = bp.add<oscillator>(freq);
    size_t gn = bp.add<gain>(g);
    bp.connect(osc, 0, gn, 0);
    bp.set_output(gn);
    return bp;
}

int main() {
    const size_t sr = 48000;
    const size_t bs = 128;
    const size_t blocks = 200;

    // --- single voice through the engine + offline backend ---
    XR_RUN("offline single voice RMS");
    {
        graph_blueprint bp = make_voice(440.0, 0.25);
        compiled_schedule sched = compile(bp, bs);
        XR_CHECK(sched.ok);

        engine eng;
        auto ob = std::make_unique<offline_backend>();
        offline_backend* obp = ob.get();
        eng.use_backend(std::move(ob));
        XR_CHECK(eng.init(sr, bs, 0, 2));

        instance_handle h = eng.spawn(sched);
        XR_CHECK(h.valid());

        obp->render(blocks);
        XR_CHECK_NEAR(obp->rms(0), SINE_RMS * 0.25, 0.005);
        XR_CHECK_NEAR(obp->rms(1), 0.0, 1e-9);   // only 1 output channel -> ch1 silent
        XR_CHECK(obp->peak(0) <= 0.25 + 1e-6);
    }

    // --- two voices of one blueprint sum ---
    XR_RUN("offline two-voice summing");
    {
        graph_blueprint bp = make_voice(440.0, 0.25);
        compiled_schedule sched = compile(bp, bs);
        XR_CHECK(sched.ok);

        engine eng;
        auto ob = std::make_unique<offline_backend>();
        offline_backend* obp = ob.get();
        eng.use_backend(std::move(ob));
        XR_CHECK(eng.init(sr, bs, 0, 2));

        eng.spawn(sched);
        eng.spawn(sched);

        obp->render(blocks);
        XR_CHECK_NEAR(obp->rms(0), SINE_RMS * 0.5, 0.005); // in-phase -> doubles
    }

    // --- silence ---
    XR_RUN("offline silence");
    {
        engine eng;
        auto ob = std::make_unique<offline_backend>();
        offline_backend* obp = ob.get();
        eng.use_backend(std::move(ob));
        XR_CHECK(eng.init(sr, bs, 0, 2));
        obp->render(10);
        XR_CHECK_NEAR(obp->rms(0), 0.0, 1e-12);
        XR_CHECK_NEAR(obp->peak(0), 0.0, 1e-12);
    }

    // --- master buffers are SIMD-aligned, for any block size ---
    // They come from an arena at simd_align, one aligned allocation per channel,
    // so alignment holds even for tiny blocks (a strided single-block layout
    // would only be 64-aligned once block_size * sizeof(sample_t) >= 64).
    XR_RUN("master buffers are 64-byte aligned");
    {
        for (size_t block : {4u, 8u, 32u, 128u, 1024u}) {
            engine eng;
            eng.use_backend(std::make_unique<offline_backend>());
            XR_CHECK(eng.init(sr, block, 0, 2));
            for (size_t ch = 0; ch < eng.output_channels; ++ch) {
                auto addr = reinterpret_cast<uintptr_t>(eng.master_buffers[ch]);
                XR_CHECK(addr % simd_align == 0);
            }
        }
    }

    XR_MAIN_REPORT();
}
