#include "core.hpp"
#include "standard_nodes.hpp"
#include "blueprint.hpp"
#include "schedule.hpp"
#include "instance.hpp"
#include "engine.hpp"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    using namespace xrune;
    std::cout << "--- Xrune Phase 1: Blueprint / Instance separation ---\n";

    // One blueprint: osc -> gain -> (mono->stereo) -> stereo fader.
    graph_blueprint bp;
    size_t osc   = bp.add<oscillator>(440.0);
    size_t g     = bp.add<gain>(0.25);
    size_t m2s   = bp.add<mono_to_stereo>();
    size_t fader = bp.add<stereo_fader>(0.5);
    bp.connect(osc, 0, g, 0);
    bp.connect(g, 0, m2s, 0);
    bp.connect(m2s, 0, fader, 0);
    bp.connect(m2s, 1, fader, 1);
    bp.set_output(fader);

    const size_t sr = 44100, bs = 256;
    compiled_schedule sched = compile(bp, bs);
    if (!sched.ok) { std::cerr << "Graph compilation failed!\n"; return 1; }

    engine eng;
    if (!eng.init(sr, bs, 0, 2)) { std::cerr << "Engine init failed!\n"; return 1; }

    // Two independent voices from the same blueprint.
    auto v1 = instantiate(sched, sr);
    auto v2 = instantiate(sched, sr);
    if (!v1 || !v2) { std::cerr << "Instantiation failed!\n"; return 1; }
    eng.register_instance(v1.get());
    eng.register_instance(v2.get());

    if (!eng.start()) { std::cerr << "Failed to start audio!\n"; return 1; }

    std::cout << "Two voices, both 440 Hz...\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "Detuning voice 2 -> 330 Hz (lock-free, per-instance state)...\n";
    eng.set_parameter(v2.get(), osc, 0, 330.0);
    std::this_thread::sleep_for(std::chrono::seconds(1));

    eng.stop();
    std::cout << "Done.\n";
    return 0;
}
