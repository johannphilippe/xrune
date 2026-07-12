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

#include "api.hpp"
#include "standard_nodes.hpp"
#include "offline_backend.hpp"
#include "test_util.hpp"
#include <memory>

using namespace xrune;

static constexpr double SINE_RMS = 0.70710678118654752440;
static constexpr size_t SR = 48000, BS = 128;

// Build a runtime wired to an offline backend; returns the backend pointer.
static offline_backend* make_rt(runtime& rt, size_t workers = 0) {
    auto ob = std::make_unique<offline_backend>();
    offline_backend* p = ob.get();
    rt.use_backend(std::move(ob));
    rt.init({SR, BS, 0, 2, 32, workers});
    rt.start();
    return p;
}

static blueprint_builder synth_builder(double freq, double g) {
    return std::move(build("synth")
        .add<oscillator>("osc", freq)
        .add<gain>("amp", g)
        .connect("osc", 0, "amp", 0)
        .output("amp"));
}

static blueprint_builder bus_builder(double f) {
    return std::move(build("bus")
        .add<bus_input>("in_node", 1)
        .add<gain>("fader", f)
        .connect("in_node", 0, "fader", 0)
        .input_terminal("in", "in_node")
        .output("fader"));
}

int main() {
    // --- builder error accumulation ---
    XR_RUN("builder errors");
    {
        blueprint_builder b("bad");
        b.add<oscillator>("osc", 440.0)
            .add<gain>("osc", 1.0)               // duplicate name
            .connect("osc", 0, "nope", 0)        // unknown node
            .modulate("osc", 0, "osc", "nope");  // unknown port
        XR_CHECK(!b.ok());
        XR_CHECK(b.errors.size() == 3);

        runtime rt;
        make_rt(rt);
        XR_CHECK(rt.register_blueprint(b) == invalid_blueprint);
        XR_CHECK(!rt.last_error().empty());
    }

    // --- register before init fails ---
    XR_RUN("register requires init");
    {
        runtime rt;
        blueprint_builder b = synth_builder(440.0, 0.25);
        XR_CHECK(rt.register_blueprint(b) == invalid_blueprint);
    }

    // --- introspection ---
    XR_RUN("describe");
    {
        runtime rt;
        make_rt(rt);
        blueprint_id id = rt.register_blueprint(synth_builder(440.0, 0.25));
        XR_CHECK(id != invalid_blueprint);
        XR_CHECK(rt.find_blueprint("synth") == id);

        const blueprint_info* info = rt.describe(id);
        XR_CHECK(info != nullptr);
        XR_CHECK(info->nodes.size() == 2);
        XR_CHECK(info->nodes[0].name == "osc");
        XR_CHECK(info->nodes[0].ports.size() == 1);
        XR_CHECK(info->nodes[0].ports[0].name == "freq");
        XR_CHECK_NEAR(info->nodes[0].ports[0].default_value, 440.0, 1e-12);
        XR_CHECK_NEAR(info->nodes[0].ports[0].min_value, 0.0, 1e-12);
        XR_CHECK_NEAR(info->nodes[0].ports[0].max_value, 20000.0, 1e-12);
        XR_CHECK(info->output_terminals.size() == 1);
        XR_CHECK(info->output_terminals[0] == "out");
    }

    // --- spawn to master + set by name + set via param_ref ---
    XR_RUN("spawn + set");
    {
        runtime rt;
        offline_backend* ob = make_rt(rt);
        blueprint_id id = rt.register_blueprint(synth_builder(440.0, 0.25));

        voice v = rt.spawn(id);
        XR_CHECK(v.valid());
        ob->render(200);
        XR_CHECK_NEAR(ob->rms(0), SINE_RMS * 0.25, 0.005);

        // by name
        XR_CHECK(rt.set(v, "amp", "gain", 0.5));
        ob->render(2); // settle the ramp
        ob->render(200);
        XR_CHECK_NEAR(ob->rms(0), SINE_RMS * 0.5, 0.005);

        // pre-resolved
        param_ref g = rt.resolve(id, "amp", "gain");
        XR_CHECK(g.ok);
        XR_CHECK(rt.set(v, g, 0.1));
        ob->render(2);
        ob->render(200);
        XR_CHECK_NEAR(ob->rms(0), SINE_RMS * 0.1, 0.005);

        // bad addresses fail cleanly
        XR_CHECK(!rt.set(v, "nope", "gain", 1.0));
        XR_CHECK(!rt.set(v, "amp", "nope", 1.0));
    }

    // --- spawn into a bus + runtime rerouting ---
    XR_RUN("bus routing via API");
    {
        runtime rt;
        offline_backend* ob = make_rt(rt);
        blueprint_id synth = rt.register_blueprint(synth_builder(440.0, 0.25));
        blueprint_id busbp = rt.register_blueprint(bus_builder(0.5));

        voice bus = rt.spawn(busbp);                    // bus -> master
        spawn_options into_bus; into_bus.into = bus;
        voice v = rt.spawn(synth, into_bus);            // synth -> bus
        XR_CHECK(bus.valid() && v.valid());

        ob->render(200);
        XR_CHECK_NEAR(ob->rms(0), SINE_RMS * 0.125, 0.005);   // 0.25 * bus fader 0.5

        // reroute at runtime: bypass the bus
        XR_CHECK(rt.unroute(v, bus));
        XR_CHECK(rt.route_to_master(v));
        ob->render(200);
        XR_CHECK_NEAR(ob->rms(0), SINE_RMS * 0.25, 0.005);

        // and back into the bus
        XR_CHECK(rt.unroute_from_master(v));
        XR_CHECK(rt.route(v, bus));
        ob->render(200);
        XR_CHECK_NEAR(ob->rms(0), SINE_RMS * 0.125, 0.005);
    }

    // --- modulation via builder (LFO drives a port at audio rate) ---
    XR_RUN("builder modulation");
    {
        runtime rt;
        offline_backend* ob = make_rt(rt);
        blueprint_id id = rt.register_blueprint(std::move(build("mod")
            .add<constant>("dc", 1.0)
            .add<gain>("amp", 1.0)
            .add<oscillator>("lfo", 2000.0)
            .connect("dc", 0, "amp", 0)
            .modulate("lfo", 0, "amp", "gain")
            .output("amp")));
        XR_CHECK(id != invalid_blueprint);

        rt.spawn(id);
        ob->render(200);
        // Output = 1.0 * sine (audio-rate modulated gain) -> sine RMS, not 1.0.
        XR_CHECK_NEAR(ob->rms(0), SINE_RMS, 0.01);
    }

    // --- kill / alive / pump + timed lifetime ---
    XR_RUN("lifecycle via API");
    {
        runtime rt;
        offline_backend* ob = make_rt(rt);
        blueprint_id id = rt.register_blueprint(synth_builder(440.0, 0.25));

        voice v = rt.spawn(id);
        ob->render(1);
        XR_CHECK(rt.alive(v));
        XR_CHECK(rt.active_voices() == 1);

        rt.kill(v);
        ob->render(1);
        XR_CHECK(rt.pump() == 1);
        XR_CHECK(!rt.alive(v));
        XR_CHECK(rt.active_voices() == 0);

        // timed voice reaps itself
        spawn_options timed; timed.life = {lifetime_kind::timed, 3, 1e-5, 0};
        voice t = rt.spawn(id, timed);
        ob->render(1);
        XR_CHECK(rt.alive(t));
        ob->render(4);
        rt.pump();
        XR_CHECK(!rt.alive(t));

        // time helpers
        XR_CHECK(rt.blocks(1.0) == SR / BS);
        XR_CHECK(rt.for_seconds(1.0).ttl_blocks == SR / BS);
    }

    // --- block size must be a power of two ---
    XR_RUN("block size validation");
    {
        runtime rt;
        XR_CHECK(!rt.init({SR, 100, 0, 2}));   // 100 is not a power of two
        XR_CHECK(rt.last_error().find("power of two") != std::string::npos);
        XR_CHECK(!rt.init({SR, 0, 0, 2}));      // 0 rejected too

        runtime rt2;
        make_rt(rt2);                            // 128 is fine
        XR_CHECK(rt2.config().block_size == BS);
    }

    XR_MAIN_REPORT();
}
