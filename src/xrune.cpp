#include "api.hpp"
#include "standard_nodes.hpp"
#include <iostream>
#include <thread>
#include <chrono>

// Demo of the Phase 9 control API: build a blueprint fluently, spawn voices,
// automate parameters by name, route through a bus, and manage lifetimes —
// exactly the surface Idyl will drive.

int main() {
    using namespace xrune;
    std::cout << "--- Xrune control API demo ---\n";

    runtime rt;
    if (!rt.init({44100, 256, 0, 2, 64, 0})) {
        std::cerr << "init failed: " << rt.last_error() << "\n";
        return 1;
    }

    blueprint_id synth = rt.register_blueprint(build("synth")
        .add<oscillator>("osc", 440.0)
        .add<gain>("amp", 0.2)
        .add<mono_to_stereo>("st")
        .connect("osc", 0, "amp", 0)
        .connect("amp", 0, "st", 0)
        .output("st"));

    blueprint_id busbp = rt.register_blueprint(build("bus")
        .add<bus_input>("in_node", 2)
        .add<stereo_fader>("fader", 0.6)
        .connect("in_node", 0, "fader", 0)
        .connect("in_node", 1, "fader", 1)
        .input_terminal("in", "in_node")
        .output("fader"));

    if (synth == invalid_blueprint || busbp == invalid_blueprint) {
        std::cerr << "register failed: " << rt.last_error() << "\n";
        return 1;
    }

    if (!rt.start()) { std::cerr << "start failed\n"; return 1; }

    // A permanent bus, and two voices routed into it.
    voice bus = rt.spawn(busbp);
    spawn_options into_bus; into_bus.into = bus;
    voice v1 = rt.spawn(synth, into_bus);
    voice v2 = rt.spawn(synth, into_bus);

    std::cout << "Two voices through the bus (440 Hz)...\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "Detune voice 2 by name: osc.freq = 660...\n";
    rt.set(v2, "osc", "freq", 660.0);
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "Fade the whole bus down...\n";
    rt.set(bus, "fader", "volume", 0.2);
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "Kill voice 1; spawn a 0.5 s timed voice at 880 Hz...\n";
    rt.kill(v1);
    spawn_options timed = into_bus;
    timed.life = rt.for_seconds(0.5);
    voice v3 = rt.spawn(synth, timed);
    rt.set(v3, "osc", "freq", 880.0);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    rt.pump();

    std::cout << "Voices alive: v1=" << rt.alive(v1) << " v2=" << rt.alive(v2)
              << " v3=" << rt.alive(v3) << "\n";

    rt.stop();
    std::cout << "Done.\n";
    return 0;
}
