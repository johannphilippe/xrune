#include "core.hpp"
#include "graph.hpp"
#include "standard_nodes.hpp"
#include "engine.hpp"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "--- Xrune Stage 3 Real-Time Safety & Lock-Free IPC ---" << std::endl;

    xrune::oscillator osc_l(440.0);
    xrune::oscillator osc_r(660.0);
    xrune::gain gain_l(0.25);
    xrune::gain gain_r(0.25);
    xrune::stereo_fader fader(0.4);

    xrune::graph g;
    g.add_node(&osc_l);
    g.add_node(&osc_r);
    g.add_node(&gain_l);
    g.add_node(&gain_r);
    g.add_node(&fader);

    g.connect(&osc_l, 0, &gain_l, 0);
    g.connect(&gain_l, 0, &fader, 0);

    g.connect(&osc_r, 0, &gain_r, 0);
    g.connect(&gain_r, 0, &fader, 1);

    g.output_node = &fader;

    std::cout << "Compiling graph blueprint on main thread..." << std::endl;
    if (!g.compile()) {
        std::cerr << "Graph compilation failed!" << std::endl;
        return 1;
    }
    std::cout << "Graph compiled successfully." << std::endl;

    xrune::engine eng;
    size_t sample_rate = 44100;
    size_t block_size = 256;
    size_t input_channels = 0;
    size_t output_channels = 2;

    if (!eng.init(sample_rate, block_size, input_channels, output_channels)) {
        std::cerr << "Engine initialization failed!" << std::endl;
        return 1;
    }

    std::cout << "Registering graph to engine (lock-free)..." << std::endl;
    eng.register_graph(&g);

    std::cout << "Starting audio stream..." << std::endl;
    if (!eng.start()) {
        std::cerr << "Failed to start audio stream!" << std::endl;
        return 1;
    }

    // Play initial frequencies
    std::cout << "Playing Initial Frequencies (Left: 440 Hz, Right: 660 Hz)..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Dynamic parameter updates (frequency sweeps via SPSC command queue)
    std::cout << "Sweeping frequencies via lock-free parameter commands..." << std::endl;
    std::cout << "New targets -> Left: 220 Hz, Right: 330 Hz" << std::endl;
    eng.set_parameter(&osc_l, 0, 220.0);
    eng.set_parameter(&osc_r, 0, 330.0);
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Flag graph completion (simulating lifecycle end)
    std::cout << "Signaling graph completion (finished_flag)..." << std::endl;
    g.finished_flag = true;

    // Wait a brief moment and check telemetry/cleanup queue
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    xrune::graph* completed = nullptr;
    if (eng.dequeue_completed_graph(completed)) {
        std::cout << "Successfully dequeued completed graph pointer [address: " 
                  << completed << "] on the main thread for safe garbage collection!" << std::endl;
    } else {
        std::cout << "No completed graphs found in telemetry queue." << std::endl;
    }

    // Sound should be off now (since graph was removed from active_graphs in the callback)
    std::cout << "Sound should be silent now. Waiting for 1 second..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "Stopping stream..." << std::endl;
    eng.stop();
    std::cout << "Engine stopped. Done." << std::endl;

    return 0;
}
