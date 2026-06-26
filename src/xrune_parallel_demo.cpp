#include "core.hpp"
#include "graph.hpp"
#include "standard_nodes.hpp"
#include "engine.hpp"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "--- Xrune Stage 4: Parallel Data-Flow Execution ---" << std::endl;

    // Create a larger graph to trigger parallel execution
    // This graph has 8 nodes: 4 oscillators -> 4 gains -> 1 mixer -> 1 output fader
    xrune::oscillator osc1(220.0);
    xrune::oscillator osc2(440.0);
    xrune::oscillator osc3(660.0);
    xrune::oscillator osc4(880.0);
    
    xrune::gain gain1(0.15);
    xrune::gain gain2(0.15);
    xrune::gain gain3(0.15);
    xrune::gain gain4(0.15);
    
    xrune::mixer mixer;
    xrune::stereo_fader master_fader(0.5);

    xrune::graph g;
    
    // Add all nodes
    g.add_node(&osc1);
    g.add_node(&osc2);
    g.add_node(&osc3);
    g.add_node(&osc4);
    g.add_node(&gain1);
    g.add_node(&gain2);
    g.add_node(&gain3);
    g.add_node(&gain4);
    g.add_node(&mixer);
    g.add_node(&master_fader);

    // Connect oscillators to gains (parallel branches)
    g.connect(&osc1, 0, &gain1, 0);
    g.connect(&osc2, 0, &gain2, 0);
    g.connect(&osc3, 0, &gain3, 0);
    g.connect(&osc4, 0, &gain4, 0);
    
    // Connect gains to mixer (4 parallel inputs)
    g.connect(&gain1, 0, &mixer, 0);
    g.connect(&gain2, 0, &mixer, 1);
    g.connect(&gain3, 0, &mixer, 2);
    g.connect(&gain4, 0, &mixer, 3);
    
    // Connect mixer to stereo fader
    // Note: mixer has 1 output, we need to connect to both inputs of stereo_fader
    // For simplicity, we'll just connect to one input and duplicate the signal
    g.connect(&mixer, 0, &master_fader, 0);
    g.connect(&mixer, 0, &master_fader, 1);

    g.output_node = &master_fader;

    std::cout << "Compiling graph with " << g.nodes.size() << " nodes..." << std::endl;
    if (!g.compile()) {
        std::cerr << "Graph compilation failed!" << std::endl;
        return 1;
    }
    std::cout << "Graph compiled successfully." << std::endl;
    std::cout << "Parallel execution enabled: " << (g.run_parallel ? "YES" : "NO") << std::endl;

    // Create engine with 4 worker threads
    size_t num_workers = 4;
    xrune::engine eng;
    size_t sample_rate = 44100;
    size_t block_size = 256;
    size_t input_channels = 0;
    size_t output_channels = 2;

    std::cout << "Initializing engine with " << num_workers << " worker threads..." << std::endl;
    if (!eng.init(sample_rate, block_size, input_channels, output_channels, num_workers)) {
        std::cerr << "Engine initialization failed!" << std::endl;
        return 1;
    }

    std::cout << "Registering graph to engine (lock-free)..." << std::endl;
    eng.register_graph(&g);

    std::cout << "Starting audio stream with parallel execution..." << std::endl;
    if (!eng.start()) {
        std::cerr << "Failed to start audio stream!" << std::endl;
        return 1;
    }

    // Play the parallel graph
    std::cout << "Playing parallel graph (4 oscillators mixed together)..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Sweep frequencies to demonstrate dynamic parameter updates
    std::cout << "Sweeping oscillator frequencies..." << std::endl;
    eng.set_parameter(&osc1, 0, 440.0);
    eng.set_parameter(&osc2, 0, 880.0);
    eng.set_parameter(&osc3, 0, 1320.0);
    eng.set_parameter(&osc4, 0, 1760.0);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Flag graph completion
    std::cout << "Signaling graph completion..." << std::endl;
    g.finished_flag = true;

    // Wait a brief moment and check telemetry/cleanup queue
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    xrune::graph* completed = nullptr;
    if (eng.dequeue_completed_graph(completed)) {
        std::cout << "Successfully dequeued completed graph [address: " 
                  << completed << "] for safe garbage collection!" << std::endl;
    } else {
        std::cout << "No completed graphs found in telemetry queue." << std::endl;
    }

    // Sound should be off now
    std::cout << "Sound should be silent now. Waiting..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "Stopping stream..." << std::endl;
    eng.stop();
    std::cout << "Engine stopped. Parallel execution test complete!" << std::endl;

    return 0;
}
