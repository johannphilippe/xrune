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

    // Two independent voices spawned from the same blueprint (handle-addressed).
    instance_handle v1 = eng.spawn(sched);
    instance_handle v2 = eng.spawn(sched);
    if (!v1.valid() || !v2.valid()) { std::cerr << "Spawn failed!\n"; return 1; }

    if (!eng.start()) { std::cerr << "Failed to start audio!\n"; return 1; }

    std::cout << "Two voices, both 440 Hz...\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "Detuning voice 2 -> 330 Hz (lock-free, handle-addressed)...\n";
    eng.set_parameter(v2, osc, 0, 330.0);
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "Killing voice 1; spawning a timed voice (0.5s)...\n";
    eng.kill(v1);
    lifetime_policy timed{lifetime_kind::timed, static_cast<size_t>(0.5 * sr / bs), 1e-5, 0};
    eng.spawn(sched, timed);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    eng.reclaim();

    eng.stop();
    std::cout << "Done.\n";
    return 0;
}
