#include "core.hpp"
#include "standard_nodes.hpp"
#include "blueprint.hpp"
#include "schedule.hpp"
#include "instance.hpp"
#include "instance_manager.hpp"
#include "engine.hpp"
#include "offline_backend.hpp"
#include "test_util.hpp"
#include <memory>
#include <cmath>

using namespace xrune;

static constexpr double SINE_RMS = 0.70710678118654752440;

// Signal = constant(1) -> gain(g); output = gain's port value (easy to observe).
static graph_blueprint make_gain_probe(double g) {
    graph_blueprint bp;
    size_t c = bp.add_named<constant>("src", 1.0);
    size_t a = bp.add_named<gain>("amp", g);
    bp.connect(c, 0, a, 0);
    bp.set_output(a);
    return bp;
}

int main() {
    const size_t sr = 48000, bs = 128;

    // --- name resolution + typed defaults ---
    XR_RUN("port name resolution + default");
    {
        graph_blueprint bp = make_gain_probe(0.5);
        XR_CHECK(bp.find_param(bp.find_node("amp"), "gain") == 0);
        XR_CHECK(bp.find_param(bp.find_node("src"), "value") == 0);
        XR_CHECK(bp.find_param(bp.find_node("amp"), "nope") == -1);

        compiled_schedule sched = compile(bp, bs);
        XR_CHECK(sched.total_params == 2);   // constant.value + gain.gain
        auto v = instantiate(sched, sr);
        v->process();
        audio_buffer_view out = v->output_view(0);
        for (size_t i = 0; i < out.size; ++i) XR_CHECK_NEAR(out[i], 0.5, 1e-12); // default applied
    }

    // --- set a value, control-rate (click-free one-block ramp to target) ---
    XR_RUN("control-rate set with smoothing");
    {
        graph_blueprint bp = make_gain_probe(1.0);
        size_t amp = bp.find_node("amp");
        compiled_schedule sched = compile(bp, bs);
        auto v = instantiate(sched, sr);

        v->process();
        XR_CHECK_NEAR(v->output_view(0)[0], 1.0, 1e-12);        // at default

        v->set_parameter(amp, 0, 0.0);
        v->process();                                            // ramp 1.0 -> 0.0
        audio_buffer_view r = v->output_view(0);
        XR_CHECK_NEAR(r[0], 1.0, 1e-9);                          // starts at old value (no click)
        XR_CHECK(r[bs - 1] < 0.05);                              // ramped down within the block
        XR_CHECK(r[bs - 1] > 0.0);                               // but not instantaneous

        v->process();                                            // now settled at target
        audio_buffer_view r2 = v->output_view(0);
        for (size_t i = 0; i < r2.size; ++i) XR_CHECK_NEAR(r2[i], 0.0, 1e-12);
    }

    // --- audio-rate modulation: connect an oscillator to the gain port ---
    XR_RUN("audio-rate modulation via connection");
    {
        graph_blueprint bp;
        size_t src = bp.add<constant>(1.0);
        size_t amp = bp.add<gain>(1.0);
        size_t lfo = bp.add<oscillator>(440.0);
        bp.connect(src, 0, amp, 0);
        bp.connect_param(lfo, 0, amp, 0);   // osc drives "gain" at audio rate
        bp.set_output(amp);

        compiled_schedule sched = compile(bp, bs);
        XR_CHECK(sched.ok);
        // osc must be scheduled before the gain it modulates.
        XR_CHECK(sched.param_source[amp][0] != SILENT_SLOT);

        auto v = instantiate(sched, sr);

        double sumsq = 0.0; size_t count = 0;
        sample_t bmin = 1e9, bmax = -1e9;
        for (int blk = 0; blk < 200; ++blk) {
            v->process();
            audio_buffer_view o = v->output_view(0);
            for (size_t i = 0; i < o.size; ++i) {
                sumsq += o[i] * o[i]; ++count;
                if (blk == 0) { bmin = std::min(bmin, o[i]); bmax = std::max(bmax, o[i]); }
            }
        }
        // Output = 1.0 * sine -> per-sample sine (RMS ~0.707), and varies within
        // a block (a control-rate read would be constant per block).
        XR_CHECK_NEAR(std::sqrt(sumsq / count), SINE_RMS, 0.005);
        XR_CHECK((bmax - bmin) > 1.5);
    }

    // --- engine command path: handle-addressed port set ---
    XR_RUN("engine set_parameter via command");
    {
        graph_blueprint bp = make_gain_probe(1.0);
        size_t amp = bp.find_node("amp");
        compiled_schedule sched = compile(bp, bs);

        engine eng;
        auto ob = std::make_unique<offline_backend>();
        offline_backend* obp = ob.get();
        eng.use_backend(std::move(ob));
        eng.init(sr, bs, 0, 2, 8);

        instance_handle h = eng.spawn(sched);
        obp->render(50);
        XR_CHECK_NEAR(obp->rms(0), 1.0, 1e-6);            // constant 1.0 * gain 1.0

        eng.set_parameter(h, amp, 0, 0.25);
        obp->render(2);                                    // let the ramp settle
        obp->render(50);
        XR_CHECK_NEAR(obp->rms(0), 0.25, 1e-6);           // gain now 0.25
    }

    XR_MAIN_REPORT();
}
