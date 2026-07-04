#include "galdr/compile.hpp"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

// galdr_play — load a .rune file and play one of its runes through the audio
// device. Usage:
//   galdr_play [file.rune] [rune-name] [seconds]
// With no file, plays a built-in demo.

static const char* kBuiltin =
    "sigil detuned(f, spread)\n"
    "  sine(freq = f) , sine(freq = f * (1 + spread))\n"
    "end\n"
    "rune demo(base = 110)\n"
    "  out detuned(base, 0.008) :> gain(0.12) : m2s\n"
    "end\n";

int main(int argc, char** argv) {
    using namespace xrune;

    const std::string file = (argc >= 2) ? argv[1] : "";
    const std::string want = (argc >= 3) ? argv[2] : "";
    const int seconds = (argc >= 4) ? std::atoi(argv[3]) : 3;

    runtime rt;
    if (!rt.init({48000, 256, 0, 2, 64, 0})) {
        std::cerr << "engine init failed: " << rt.last_error() << "\n";
        return 1;
    }

    galdr::load_result r = file.empty() ? galdr::load(rt, kBuiltin)
                                        : galdr::load_file(rt, file);
    if (!r.ok()) {
        std::cerr << (file.empty() ? "<builtin>" : file) << ":\n";
        for (const auto& d : r.diags) std::cerr << "  " << d.format() << "\n";
        return 1;
    }
    if (r.blueprints.empty()) {
        std::cerr << "no runes found\n";
        return 1;
    }

    blueprint_id id = want.empty() ? r.blueprints.front().second : r.find(want);
    std::string name = want.empty() ? r.blueprints.front().first : want;
    if (id == invalid_blueprint) {
        std::cerr << "no rune named '" << want << "'\n";
        return 1;
    }

    std::cout << "Loaded " << r.blueprints.size() << " rune(s). Playing '" << name
              << "' for " << seconds << "s...\n";

    if (!rt.start()) { std::cerr << "audio start failed\n"; return 1; }
    voice v = rt.spawn(id);
    if (!v.valid()) { std::cerr << "spawn failed: " << rt.last_error() << "\n"; return 1; }

    std::this_thread::sleep_for(std::chrono::seconds(seconds));

    rt.kill(v);
    rt.stop();
    std::cout << "Done.\n";
    return 0;
}
