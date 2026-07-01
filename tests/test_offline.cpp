#include "core.hpp"
#include "standard_nodes.hpp"
#include "blueprint.hpp"
#include "schedule.hpp"
#include "instance.hpp"
#include "engine.hpp"
#include "offline_backend.hpp"
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

        auto v = instantiate(sched, sr);
        XR_CHECK(v != nullptr);
        eng.register_instance(v.get());

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

        auto a = instantiate(sched, sr);
        auto b = instantiate(sched, sr);
        eng.register_instance(a.get());
        eng.register_instance(b.get());

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

    XR_MAIN_REPORT();
}
