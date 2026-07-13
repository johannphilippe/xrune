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

// xrune — the command-line front-end.
//
//   xrune patch.rune                 run it through the audio device
//   xrune -e patch.rune              check that it compiles (no audio)
//   xrune -j patch.rune              emit the blueprint as JSON
//   xrune -d patch.rune              emit Graphviz DOT
//   xrune -p patch.rune -o g.png     render a PNG (needs Graphviz's `dot`)
//
// Every mode except `run` uses the offline backend, so nothing opens an audio
// device: eval and the exporters work headless (CI, servers, an editor shelling
// out to us).

#include <xrune/api.hpp>
#include <xrune/serialize.hpp>
#include <xrune/lang/compile.hpp>
#include <xrune/audio/offline_backend.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifndef XRUNE_VERSION
#define XRUNE_VERSION "0.0.0"
#endif

using namespace xrune;

namespace {

enum class mode { run, eval, json, dot, png, list };

struct options {
    mode action = mode::run;
    std::string file;
    std::string rune;       // which rune (default: the first defined)
    std::string output;     // "" = stdout (png derives a name instead)
    int seconds = 5;
    size_t sample_rate = 48000;
    size_t block_size = 256;
    int indent = 2;         // 0 = compact JSON
};

void usage() {
    std::cout <<
"xrune " XRUNE_VERSION " — real-time audio engine and its language\n"
"\n"
"USAGE\n"
"  xrune [mode] [options] <file.rune>\n"
"\n"
"MODES  (default: run)\n"
"  (none)            run the rune through the audio device\n"
"  -e, --eval        parse and compile only; report diagnostics; no audio\n"
"  -j, --json        emit the blueprint as JSON (reloadable)\n"
"  -d, --dot         emit a Graphviz DOT graph\n"
"  -p, --png         render a PNG graph (needs Graphviz's `dot` on PATH)\n"
"  -l, --list        list the runes defined in the file\n"
"\n"
"OPTIONS\n"
"  -r, --rune NAME     which rune to use (default: the first defined)\n"
"  -o, --output FILE   write to FILE (default: stdout; --png: <file>.png)\n"
"  -s, --seconds N     how long to play, in run mode (default: 5)\n"
"      --sample-rate N (default: 48000)\n"
"      --block-size N  power of two (default: 256)\n"
"      --compact       JSON on a single line\n"
"  -h, --help\n"
"  -v, --version\n"
"\n"
"EXAMPLES\n"
"  xrune examples/drone.rune                    # play it\n"
"  xrune -e examples/drone.rune                 # is it valid?\n"
"  xrune -j examples/drone.rune -o patch.json   # save it\n"
"  xrune -d examples/drone.rune | dot -Tsvg -o patch.svg\n"
"  xrune -p examples/drone.rune -o patch.png    # straight to PNG\n"
"\n"
"EXIT STATUS\n"
"  0 on success; 1 on a compile error, an unknown rune, or a failed render.\n";
}

// Render DOT to PNG by piping it into Graphviz's `dot`.
//
// Deliberately NOT linked against libgvc: Graphviz's C API would pull a heavy
// build-time dependency into a project that is otherwise self-contained, to do
// something the `dot` binary already does perfectly. The price is that PNG needs
// Graphviz installed — so we detect its absence and say exactly that.
bool render_png(const std::string& dot_text, const std::string& out_path) {
    // Single-quote the path for the shell, escaping any embedded quote.
    std::string quoted = "'";
    for (char c : out_path) quoted += (c == '\'') ? std::string("'\\''") : std::string(1, c);
    quoted += "'";

    const std::string cmd = "dot -Tpng -o " + quoted + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "w");
    if (!pipe) {
        std::cerr << "xrune: could not run Graphviz (`dot`)\n";
        return false;
    }
    std::fwrite(dot_text.data(), 1, dot_text.size(), pipe);
    if (pclose(pipe) != 0) {
        std::cerr <<
            "xrune: PNG rendering failed.\n"
            "  This needs Graphviz's `dot` on your PATH:\n"
            "    Debian/Ubuntu : sudo apt install graphviz\n"
            "    macOS         : brew install graphviz\n"
            "  Or emit DOT and render it elsewhere:  xrune -d file.rune\n";
        return false;
    }
    return true;
}

void write_out(const std::string& text, const std::string& path) {
    if (path.empty()) { std::cout << text; return; }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::cerr << "xrune: cannot write: " << path << "\n"; std::exit(1); }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    // stderr, so a redirected stdout stays pure data.
    std::cerr << "xrune: wrote " << path << "\n";
}

// Directory and extension stripped — used to derive a default output name.
std::string stem(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

bool parse_args(int argc, char** argv, options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "xrune: " << a << " needs " << what << "\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if      (a == "-h" || a == "--help")    { usage(); std::exit(0); }
        else if (a == "-v" || a == "--version") { std::cout << "xrune " XRUNE_VERSION "\n"; std::exit(0); }
        else if (a == "-e" || a == "--eval")    o.action = mode::eval;
        else if (a == "-j" || a == "--json")    o.action = mode::json;
        else if (a == "-d" || a == "--dot")     o.action = mode::dot;
        else if (a == "-p" || a == "--png")     o.action = mode::png;
        else if (a == "-l" || a == "--list")    o.action = mode::list;
        else if (a == "-r" || a == "--rune")    o.rune   = next("a rune name");
        else if (a == "-o" || a == "--output")  o.output = next("a file path");
        else if (a == "-s" || a == "--seconds") o.seconds = std::atoi(next("a duration").c_str());
        else if (a == "--sample-rate") o.sample_rate = std::strtoul(next("a rate").c_str(), nullptr, 10);
        else if (a == "--block-size")  o.block_size  = std::strtoul(next("a size").c_str(), nullptr, 10);
        else if (a == "--compact")     o.indent = 0;
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "xrune: unknown option: " << a << "  (try --help)\n";
            return false;
        }
        else if (o.file.empty()) o.file = a;
        else {
            std::cerr << "xrune: unexpected argument: " << a << "\n";
            return false;
        }
    }
    if (o.file.empty()) {
        std::cerr << "xrune: no input file  (try --help)\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    options o;
    if (!parse_args(argc, argv, o)) return 1;

    runtime rt;

    // Every mode but `run` stays off the audio hardware, so eval and the
    // exporters work on a machine with no sound card at all.
    if (o.action != mode::run)
        rt.use_backend(std::make_unique<offline_backend>());

    if (!rt.init({o.sample_rate, o.block_size, 0, 2, 64, 0})) {
        std::cerr << "xrune: " << rt.last_error() << "\n";
        return 1;
    }

    // Parse -> lower -> compile. Diagnostics carry line:col.
    lang::load_result r = lang::load_file(rt, o.file);
    if (!r.ok()) {
        for (const auto& d : r.diags) {
            if (d.line == 0 && d.col == 0) std::cerr << "xrune: " << d.message << "\n";
            else                           std::cerr << o.file << ":" << d.format() << "\n";
        }
        std::cerr << "xrune: " << r.diags.size() << " error(s)\n";
        return 1;
    }
    if (r.blueprints.empty()) {
        std::cerr << "xrune: " << o.file << " defines no runes\n";
        return 1;
    }

    // ---- Modes that report on the whole file -------------------------------
    if (o.action == mode::eval) {
        std::cout << "OK — " << o.file << ": " << r.blueprints.size() << " rune(s)\n";
        for (const auto& entry : r.blueprints) {
            const blueprint_info* bi = rt.describe(entry.second);
            std::cout << "  " << entry.first << "  ("
                      << bi->nodes.size() << " nodes, "
                      << bi->output_terminals.size() << " output terminal(s))\n";
        }
        return 0;
    }
    if (o.action == mode::list) {
        for (const auto& entry : r.blueprints) {
            const blueprint_info* bi = rt.describe(entry.second);
            std::cout << entry.first << "\t" << bi->nodes.size() << " nodes\n";
        }
        return 0;
    }

    // ---- Modes that act on ONE rune ----------------------------------------
    const std::string name = o.rune.empty() ? r.blueprints.front().first  : o.rune;
    const blueprint_id id  = o.rune.empty() ? r.blueprints.front().second : r.find(o.rune);
    if (id == invalid_blueprint) {
        std::cerr << "xrune: no rune named '" << o.rune << "' in " << o.file << "\n"
                  << "  available:";
        for (const auto& entry : r.blueprints) std::cerr << " " << entry.first;
        std::cerr << "\n";
        return 1;
    }
    const graph_blueprint& bp = rt.registry[id]->bp;

    switch (o.action) {
        case mode::json:
            write_out(to_json(bp, name, o.indent) + "\n", o.output);
            return 0;

        case mode::dot:
            write_out(to_dot(bp, name), o.output);
            return 0;

        case mode::png: {
            const std::string out = o.output.empty() ? (stem(o.file) + ".png") : o.output;
            if (!render_png(to_dot(bp, name), out)) return 1;
            std::cerr << "xrune: wrote " << out << "\n";
            return 0;
        }

        case mode::run: {
            std::cout << "xrune: playing '" << name << "' from " << o.file
                      << " for " << o.seconds << "s\n";
            if (!rt.start()) { std::cerr << "xrune: could not start audio\n"; return 1; }
            voice v = rt.spawn(id);
            if (!v.valid()) {
                std::cerr << "xrune: spawn failed: " << rt.last_error() << "\n";
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::seconds(o.seconds));
            rt.kill(v);
            rt.stop();
            std::cout << "xrune: done\n";
            return 0;
        }

        default:
            return 1;
    }
}
