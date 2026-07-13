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
#include "test_util.hpp"
#include <memory>
#include <cmath>

using namespace xrune;

int main() {
    const size_t sr = 48000, bs = 128;

    // --- single-rate graphs are unchanged (all rate 1, one call each) ---
    XR_RUN("single-rate unaffected");
    {
        graph_blueprint bp;
        size_t c = bp.add<constant>(0.5);
        size_t g = bp.add<gain>(1.0);
        bp.connect(c, 0, g, 0);
        bp.set_output(g);
        compiled_schedule s = compile(bp, bs);
        XR_CHECK(s.ok);
        XR_CHECK(s.region_rate[c] == 1 && s.region_rate[g] == 1);
        XR_CHECK(s.total_calls == 2);
    }

    // --- oversampled region: const -> base_cnt -> up(x2) -> cnt -> down(/2) -> out ---
    XR_RUN("rate propagation + call counts");
    graph_blueprint bp;
    size_t src      = bp.add<constant>(0.5);
    size_t base_cnt = bp.add<call_counter>();
    size_t up       = bp.add<upsampler2>();
    size_t cnt      = bp.add<call_counter>();
    size_t dn       = bp.add<downsampler2>();
    bp.connect(src, 0, base_cnt, 0);
    bp.connect(base_cnt, 0, up, 0);
    bp.connect(up, 0, cnt, 0);
    bp.connect(cnt, 0, dn, 0);
    bp.set_output(dn);

    compiled_schedule s = compile(bp, bs);
    XR_CHECK(s.ok);
    XR_CHECK(s.region_rate[src] == 1);
    XR_CHECK(s.region_rate[base_cnt] == 1);
    XR_CHECK(s.region_rate[up] == 2);       // rate boundary raises region to x2
    XR_CHECK(s.region_rate[cnt] == 2);      // stays x2 through the region
    XR_CHECK(s.region_rate[dn] == 1);       // downsampler brings it back
    XR_CHECK(s.out_stride[cnt] == 2 * bs);  // x2 node has a 2B buffer per channel

    auto v = instantiate(s, sr);
    XR_CHECK(v != nullptr);

    const size_t cycles = 10;
    for (size_t i = 0; i < cycles; ++i) v->process();

    // The base counter is called once per cycle; the x2 counter twice per cycle.
    auto* base_st = static_cast<call_counter::st*>(v->state_ptr(base_cnt));
    auto* cnt_st  = static_cast<call_counter::st*>(v->state_ptr(cnt));
    XR_CHECK(base_st->calls == cycles);
    XR_CHECK(cnt_st->calls == cycles * 2);

    // --- DC round-trips through up->down (after warmup, exactly preserved) ---
    XR_RUN("oversampling round-trip preserves DC");
    {
        audio_buffer_view out = v->output_view(0);
        XR_CHECK(out.size == bs);           // output node is back at base rate
        for (size_t i = 0; i < out.size; ++i) XR_CHECK_NEAR(out[i], 0.5, 1e-9);
    }

    // --- low-frequency sine survives up->down (half-band preserves passband) ---
    XR_RUN("oversampling round-trip preserves a sine");
    {
        graph_blueprint b;
        size_t o  = b.add<oscillator>(500.0);  // well below base Nyquist/2
        size_t up = b.add<upsampler2>();
        size_t dn = b.add<downsampler2>();
        b.connect(o, 0, up, 0);
        b.connect(up, 0, dn, 0);
        b.set_output(dn);
        compiled_schedule sd = compile(b, bs);
        auto vv = instantiate(sd, sr);

        double sumsq = 0.0; size_t count = 0;
        for (int blk = 0; blk < 400; ++blk) {
            vv->process();
            if (blk < 40) continue;             // skip filter warm-up
            audio_buffer_view o0 = vv->output_view(0);
            for (size_t i = 0; i < o0.size; ++i) { sumsq += o0[i] * o0[i]; ++count; }
        }
        double rms = std::sqrt(sumsq / static_cast<double>(count));
        XR_CHECK_NEAR(rms, 0.70710678118654752440, 0.02); // amplitude preserved
    }

    // --- block-size axis: downbloc halves the block, doubles the calls ---
    XR_RUN("block-size decrease (downbloc)");
    {
        graph_blueprint b;
        size_t src2 = b.add<oscillator>(1000.0);
        size_t db   = b.add<downbloc>();            // region below runs at B/2
        size_t fine = b.add<call_counter>();        // in the finer-block region
        size_t g2   = b.add<gain>(1.0);
        b.connect(src2, 0, db, 0);
        b.connect(db, 0, fine, 0);
        b.connect(fine, 0, g2, 0);
        b.set_output(g2);

        compiled_schedule sd = compile(b, bs);
        XR_CHECK(sd.ok);
        XR_CHECK(sd.region_rate[fine] == 1);        // same sample rate...
        XR_CHECK(sd.node_block[src2] == bs);
        XR_CHECK(sd.node_block[db]   == bs / 2);    // ...but half the block
        XR_CHECK(sd.node_block[fine] == bs / 2);
        XR_CHECK(sd.calls[fine] == 2);              // so twice the calls
        XR_CHECK(sd.out_stride[fine] == bs);        // throughput unchanged (B)

        auto vb = instantiate(sd, sr);
        const size_t cyc = 5;
        for (size_t i = 0; i < cyc; ++i) vb->process();
        auto* fst = static_cast<call_counter::st*>(vb->state_ptr(fine));
        XR_CHECK(fst->calls == cyc * 2);            // finer region called 2x/cycle

        // Signal integrity: output equals the source oscillator (identity path).
        // Rebuild a plain reference and compare RMS.
        graph_blueprint ref; size_t ro = ref.add<oscillator>(1000.0); size_t rg = ref.add<gain>(1.0);
        ref.connect(ro, 0, rg, 0); ref.set_output(rg);
        compiled_schedule rs = compile(ref, bs);
        auto vr = instantiate(rs, sr);
        for (size_t i = 0; i < cyc; ++i) vr->process();
        double e = 0.0;
        audio_buffer_view ob = vb->output_view(0), orf = vr->output_view(0);
        for (size_t i = 0; i < ob.size; ++i) e = std::max(e, std::fabs(ob[i] - orf[i]));
        XR_CHECK(e < 1e-12);                        // finer blocks: bit-identical signal
    }

    // --- block increase without oversampling is rejected (cross-cycle) ---
    XR_RUN("block increase alone is rejected");
    {
        struct upbloc_probe : node {  // block 2/1 with no oversampling => calls < 1
            size_t inputs_count() const override { return 1; }
            size_t outputs_count() const override { return 1; }
            size_t block_num() const override { return 2; }
            size_t block_den() const override { return 1; }
            void process(void*, const node_processing_context& c) const override {
                for (size_t i = 0; i < c.block_size; ++i) c.outputs[0][i] = c.inputs[0][i];
            }
        };
        graph_blueprint b;
        size_t s0 = b.add<oscillator>(440.0);
        size_t u0 = b.add<upbloc_probe>();
        b.connect(s0, 0, u0, 0);
        b.set_output(u0);
        compiled_schedule sd = compile(b, bs);
        XR_CHECK(!sd.ok);                           // needs cross-cycle support
    }

    XR_MAIN_REPORT();
}
