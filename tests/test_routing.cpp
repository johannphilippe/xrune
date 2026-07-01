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

using namespace xrune;

static constexpr double SINE_RMS = 0.70710678118654752440;

// Stereo voice: osc(freq) -> gain(g) -> mono_to_stereo, output terminal "out".
static graph_blueprint make_stereo_voice(double freq, double g) {
    graph_blueprint bp;
    size_t osc = bp.add<oscillator>(freq);
    size_t gn  = bp.add<gain>(g);
    size_t m2s = bp.add<mono_to_stereo>();
    bp.connect(osc, 0, gn, 0);
    bp.connect(gn, 0, m2s, 0);
    bp.set_output(m2s);
    return bp;
}

// Stereo bus: bus_input(2) -> stereo_fader(f); input terminal "in", output "out".
static graph_blueprint make_bus(double f) {
    graph_blueprint bp;
    size_t in = bp.add<bus_input>(2);
    size_t fad = bp.add<stereo_fader>(f);
    bp.connect(in, 0, fad, 0);
    bp.connect(in, 1, fad, 1);
    bp.add_input_terminal("in", in);
    bp.set_output(fad);
    return bp;
}

static offline_backend* attach_offline(engine& eng, size_t sr, size_t bs, size_t maxi) {
    auto ob = std::make_unique<offline_backend>();
    offline_backend* p = ob.get();
    eng.use_backend(std::move(ob));
    eng.init(sr, bs, 0, 2, maxi);
    return p;
}

int main() {
    const size_t sr = 48000, bs = 128, blocks = 200;

    // --- voices -> bus -> master ---
    XR_RUN("voices through global bus");
    {
        graph_blueprint voice_bp = make_stereo_voice(440.0, 0.25);
        graph_blueprint bus_bp = make_bus(0.5);
        compiled_schedule voice = compile(voice_bp, bs);
        compiled_schedule bus = compile(bus_bp, bs);
        XR_CHECK(voice.ok && bus.ok);

        engine eng;
        offline_backend* ob = attach_offline(eng, sr, bs, 16);

        instance_handle bh = eng.spawn(bus);                       // bus -> master
        instance_handle v1 = eng.spawn(voice, {}, route_target::to(bh));
        instance_handle v2 = eng.spawn(voice, {}, route_target::to(bh));
        XR_CHECK(bh.valid() && v1.valid() && v2.valid());

        ob->render(blocks);
        XR_CHECK(eng.active_count() == 3);
        XR_CHECK(eng.route_count() == 3);          // 2 voices->bus + bus->master

        // Two voices (0.25 each) sum to 0.5 at the bus, faded by 0.5 -> 0.25.
        XR_CHECK_NEAR(ob->rms(0), SINE_RMS * 0.25, 0.005);
        XR_CHECK_NEAR(ob->rms(1), SINE_RMS * 0.25, 0.005);  // stereo, both channels

        // --- kill one voice: bus receives half the signal ---
        XR_RUN("kill one voice -> bus halves");
        eng.kill(v1);
        ob->render(1);          // process the kill
        eng.reclaim();
        XR_CHECK(eng.active_count() == 2);
        XR_CHECK(eng.route_count() == 2);          // killed voice's route purged

        ob->render(blocks);
        XR_CHECK_NEAR(ob->rms(0), SINE_RMS * 0.125, 0.005);

        // --- kill remaining voice: bus outputs silence ---
        XR_RUN("empty bus -> silence");
        eng.kill(v2);
        ob->render(1);
        eng.reclaim();
        XR_CHECK(eng.active_count() == 1);         // just the bus
        ob->render(blocks);
        XR_CHECK_NEAR(ob->rms(0), 0.0, 1e-9);
    }

    // --- dynamic reroute: master -> bus at runtime ---
    XR_RUN("dynamic reroute master->bus");
    {
        graph_blueprint voice_bp = make_stereo_voice(440.0, 0.25);
        graph_blueprint bus_bp = make_bus(0.5);
        compiled_schedule voice = compile(voice_bp, bs);
        compiled_schedule bus = compile(bus_bp, bs);

        engine eng;
        offline_backend* ob = attach_offline(eng, sr, bs, 16);

        instance_handle bh = eng.spawn(bus);
        instance_handle v = eng.spawn(voice);      // default: straight to master

        ob->render(blocks);
        XR_CHECK_NEAR(ob->rms(0), SINE_RMS * 0.25, 0.005);   // direct, no fader

        // Reroute the voice through the bus.
        eng.disconnect(v, 0, route_target::master());
        eng.connect(v, 0, route_target::to(bh));
        ob->render(blocks);
        XR_CHECK_NEAR(ob->rms(0), SINE_RMS * 0.125, 0.005);  // now faded by the bus
    }

    XR_MAIN_REPORT();
}
