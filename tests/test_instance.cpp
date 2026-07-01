#include "core.hpp"
#include "standard_nodes.hpp"
#include "blueprint.hpp"
#include "schedule.hpp"
#include "instance.hpp"
#include "test_util.hpp"
#include <memory>
#include <cmath>

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

    XR_MAIN_REPORT();
}
