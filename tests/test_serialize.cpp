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

#include "xrune/serialize.hpp"
#include "xrune/util/json.hpp"
#include "xrune/schedule.hpp"
#include "xrune/instance.hpp"
#include "xrune/node/standard_nodes.hpp"
#include "xrune/node/fft.hpp"
#include "test_util.hpp"
#include <fstream>

#include <string>

using namespace xrune;

static constexpr size_t SR = 48000, BS = 128;

// A blueprint exercising: ports with non-default values, a structural node
// (mixer input count), audio-rate modulation, and named terminals.
static graph_blueprint make_patch() {
    graph_blueprint bp;
    size_t osc  = bp.add_named<oscillator>("osc", 440.0);
    size_t lfo  = bp.add_named<oscillator>("lfo", 3.0);
    size_t amp  = bp.add_named<gain>("amp", 0.25);
    size_t dc   = bp.add_named<constant>("dc", 0.5);
    size_t mix  = bp.add_named<mixer>("mix", 3);
    size_t p    = bp.add_named<pan>("p", -0.3);

    bp.connect(osc, 0, amp, 0);
    bp.connect(amp, 0, mix, 0);
    bp.connect(dc,  0, mix, 1);
    bp.connect(osc, 0, mix, 2);
    bp.connect(mix, 0, p,   0);
    bp.connect_param(lfo, 0, amp, 0);          // LFO -> amp.gain (audio rate)
    bp.set_output(p);
    bp.add_output_terminal("aux", mix);
    return bp;
}

int main() {
    lang::node_registry reg = lang::standard_registry();

    // --- the JSON layer itself -------------------------------------------
    XR_RUN("json parse/dump round-trip");
    {
        const std::string src =
            R"({"a":1,"b":-2.5,"c":"x\"y","d":[1,2,{"e":true}],"f":null})";
        json::value v;
        std::string err;
        XR_CHECK(json::parse(src, v, err));
        XR_CHECK(err.empty());
        XR_CHECK(v.is_object());
        XR_CHECK(v.integer("a") == 1);
        XR_CHECK_NEAR(v.num("b"), -2.5, 1e-15);
        XR_CHECK(v.text("c") == "x\"y");              // escape survived
        const json::value* d = v.find("d");
        XR_CHECK(d && d->is_array() && d->arr.size() == 3);
        XR_CHECK(d->arr[2].find("e")->boolean == true);

        // dump -> parse -> same values
        json::value v2;
        XR_CHECK(json::parse(json::dump(v), v2, err));
        XR_CHECK(v2.text("c") == "x\"y");
        XR_CHECK_NEAR(v2.num("b"), -2.5, 1e-15);

        // integers stay integers (no "1024.0" in the output)
        json::value n = json::value(1024.0);
        XR_CHECK(json::dump(n) == "1024");

        // malformed input is rejected, not silently accepted
        json::value bad;
        XR_CHECK(!json::parse("{\"a\":}", bad, err));
        XR_CHECK(!err.empty());
        XR_CHECK(!json::parse("{\"a\":1} trailing", bad, err));
    }

    // --- blueprint -> JSON -> blueprint: topology --------------------------
    XR_RUN("json round-trip: topology");
    {
        graph_blueprint bp = make_patch();
        const std::string text = to_json(bp, "patch");

        graph_blueprint re;
        std::string err, name;
        XR_CHECK(from_json(text, reg, re, err, &name));
        XR_CHECK(err.empty());
        XR_CHECK(name == "patch");

        XR_CHECK(re.nodes.size() == bp.nodes.size());
        XR_CHECK(re.connections.size() == bp.connections.size());
        XR_CHECK(re.param_connections.size() == bp.param_connections.size());
        XR_CHECK(re.output_terminals.size() == bp.output_terminals.size());
        XR_CHECK(re.output_node == bp.output_node);

        for (size_t i = 0; i < bp.nodes.size(); ++i) {
            XR_CHECK(re.names[i] == bp.names[i]);
            XR_CHECK(std::string(re.nodes[i]->type_name()) == bp.nodes[i]->type_name());
            XR_CHECK(re.nodes[i]->inputs_count()  == bp.nodes[i]->inputs_count());
            XR_CHECK(re.nodes[i]->outputs_count() == bp.nodes[i]->outputs_count());
            XR_CHECK(re.nodes[i]->params_count()  == bp.nodes[i]->params_count());
            for (size_t p = 0; p < bp.nodes[i]->params_count(); ++p)
                XR_CHECK_NEAR(re.nodes[i]->param_default(p),
                              bp.nodes[i]->param_default(p), 1e-12);
        }
        for (size_t i = 0; i < bp.connections.size(); ++i) {
            XR_CHECK(re.connections[i].src_node   == bp.connections[i].src_node);
            XR_CHECK(re.connections[i].src_output == bp.connections[i].src_output);
            XR_CHECK(re.connections[i].dst_node   == bp.connections[i].dst_node);
            XR_CHECK(re.connections[i].dst_input  == bp.connections[i].dst_input);
        }
        XR_CHECK(re.param_connections[0].src_node  == bp.param_connections[0].src_node);
        XR_CHECK(re.param_connections[0].dst_param == bp.param_connections[0].dst_param);
        XR_CHECK(re.output_terminals[1].name == "aux");
    }

    // --- the real proof: a reloaded patch renders identical audio ----------
    XR_RUN("json round-trip: bit-identical audio");
    {
        graph_blueprint bp = make_patch();
        graph_blueprint re;
        std::string err;
        XR_CHECK(from_json(to_json(bp), reg, re, err));

        compiled_schedule sa = compile(bp, BS);
        compiled_schedule sb = compile(re, BS);
        auto ia = instantiate(sa, SR);
        auto ib = instantiate(sb, SR);
        XR_CHECK(ia && ib);

        bool identical = true;
        for (int blk = 0; blk < 8; ++blk) {
            ia->process();
            ib->process();
            for (size_t ch = 0; ch < 2; ++ch) {      // pan -> stereo
                auto va = ia->output_view(ch);
                auto vb = ib->output_view(ch);
                for (size_t i = 0; i < BS; ++i)
                    if (va[i] != vb[i]) identical = false;   // exact, not near
            }
        }
        XR_CHECK(identical);
    }

    // --- structural args survive (not recoverable from ports alone) --------
    XR_RUN("json round-trip: config args");
    {
        graph_blueprint bp;
        bp.add_named<mixer>("m", 5);
        bp.add_named<channel_adapter>("a", 2, 3);
        bp.add_named<bus_input>("b", 4);
        bp.add_named<ola_stft>("s", 256);
        bp.set_output(0);

        graph_blueprint re;
        std::string err;
        XR_CHECK(from_json(to_json(bp), reg, re, err));
        XR_CHECK(err.empty());
        XR_CHECK(re.nodes.size() == 4);

        XR_CHECK(re.nodes[0]->inputs_count() == 5);                   // mixer inputs
        XR_CHECK(re.nodes[1]->inputs_count() == 2);                   // adapter in
        XR_CHECK(re.nodes[1]->outputs_count() == 3);                  // adapter out
        XR_CHECK(re.nodes[2]->outputs_count() == 4);                  // bus channels
        // fft_size is a private member: only config_args() can carry it across.
        XR_CHECK(static_cast<ola_stft*>(re.nodes[3].get())->fft_size == 256);
    }

    // --- load errors are reported, never silently swallowed ----------------
    XR_RUN("json load errors");
    {
        graph_blueprint re;
        std::string err;

        XR_CHECK(!from_json("{ not json", reg, re, err));
        XR_CHECK(!err.empty());

        XR_CHECK(!from_json(R"({"xrune":99,"nodes":[]})", reg, re, err));
        XR_CHECK(err.find("version") != std::string::npos);

        XR_CHECK(!from_json(R"({"xrune":1,"nodes":[{"id":0,"type":"nope"}]})", reg, re, err));
        XR_CHECK(err.find("unknown node type") != std::string::npos);

        // a connection to a node that doesn't exist
        XR_CHECK(!from_json(
            R"({"xrune":1,"nodes":[{"id":0,"type":"gain"}],
                "connections":[{"src":0,"out":0,"dst":7,"in":0}]})", reg, re, err));
        XR_CHECK(err.find("unknown node") != std::string::npos);

        // a second source into one input must be refused, as in C++
        XR_CHECK(!from_json(
            R"({"xrune":1,"nodes":[{"id":0,"type":"sine"},{"id":1,"type":"sine"},
                                   {"id":2,"type":"gain"}],
                "connections":[{"src":0,"out":0,"dst":2,"in":0},
                               {"src":1,"out":0,"dst":2,"in":0}]})", reg, re, err));
        XR_CHECK(err.find("invalid connection") != std::string::npos);
    }

    // --- Graphviz DOT export ----------------------------------------------
    XR_RUN("dot export");
    {
        graph_blueprint bp = make_patch();
        const std::string dot = to_dot(bp, "patch");

        XR_CHECK(dot.find("digraph \"patch\"") != std::string::npos);
        XR_CHECK(dot.find("rankdir=LR") != std::string::npos);
        XR_CHECK(dot.rfind("}\n") != std::string::npos);

        // nodes carry their name, type and port defaults
        XR_CHECK(dot.find("osc\\n<sine>") != std::string::npos);
        XR_CHECK(dot.find("amp\\n<gain>") != std::string::npos);
        XR_CHECK(dot.find("gain = 0.25") != std::string::npos);
        XR_CHECK(dot.find("<mix>") != std::string::npos);

        // an audio edge, and the modulation edge drawn dashed + labelled
        XR_CHECK(dot.find("n0 -> n2") != std::string::npos);          // osc -> amp
        XR_CHECK(dot.find("style=dashed") != std::string::npos);
        XR_CHECK(dot.find("label=\"gain\"") != std::string::npos);    // lfo -> amp.gain

        // terminals
        XR_CHECK(dot.find("out0 [label=\"out\"") != std::string::npos);
        XR_CHECK(dot.find("out1 [label=\"aux\"") != std::string::npos);
    }

    // --- a rate-changing node is highlighted as a region boundary ----------
    XR_RUN("dot marks rate boundaries");
    {
        graph_blueprint bp;
        size_t o = bp.add_named<oscillator>("o", 100.0);
        size_t u = bp.add_named<upsampler2>("up");
        bp.connect(o, 0, u, 0);
        bp.set_output(u);

        const std::string dot = to_dot(bp);
        XR_CHECK(dot.find("<up2>") != std::string::npos);
        XR_CHECK(dot.find("rate 2/1") != std::string::npos);
        std::ofstream ofs("./dot.txt"); 
        ofs << dot; 
    }
    

    XR_MAIN_REPORT();
}
