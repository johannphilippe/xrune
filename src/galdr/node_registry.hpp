#pragma once
#include "../core.hpp"
#include "../standard_nodes.hpp"
#include "../node/fft.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>

// Galdr node registry (spec §14): maps DSL type names -> node factories, so the
// compiler can instantiate nodes by name. Also the extension point for plugin /
// host nodes (register a name, get a DSL word). Isolated from the engine.

namespace xrune::galdr {

// A resolved argument value from a node call (numbers after compile-time
// arithmetic, strings, or booleans).
struct arg_value {
    enum class kind { number, string, boolean };
    kind k = kind::number;
    double number = 0.0;
    std::string str;
    bool boolean = false;

    arg_value() = default;
    arg_value(double n) : k(kind::number), number(n) {}
    static arg_value make_string(std::string s) { arg_value v; v.k = kind::string; v.str = std::move(s); return v; }
    static arg_value make_bool(bool b) { arg_value v; v.k = kind::boolean; v.boolean = b; return v; }
};

// Parsed arguments of one node call: named (name = value) and positional.
struct node_args {
    std::vector<std::pair<std::string, arg_value>> named;
    std::vector<arg_value> positional;

    const arg_value* find_named(const std::string& n) const {
        for (const auto& p : named) if (p.first == n) return &p.second;
        return nullptr;
    }
    bool has(const std::string& name) const { return find_named(name) != nullptr; }

    // Resolution order: named wins, else positional[pos], else default.
    double num(const std::string& name, size_t pos, double def) const {
        if (const auto* v = find_named(name)) return v->number;
        if (pos < positional.size() && positional[pos].k == arg_value::kind::number)
            return positional[pos].number;
        return def;
    }
    size_t count(const std::string& name, size_t pos, size_t def) const {
        double d = num(name, pos, static_cast<double>(def));
        return d < 0.0 ? def : static_cast<size_t>(d + 0.5);
    }
    std::string str(const std::string& name, const std::string& def = "") const {
        if (const auto* v = find_named(name); v && v->k == arg_value::kind::string) return v->str;
        return def;
    }
};

struct node_registry {
    using factory = std::function<std::unique_ptr<node>(const node_args&)>;
    struct entry {
        factory make;
        std::vector<std::string> structural; // arg names that configure construction (not ports)
    };
    std::unordered_map<std::string, entry> entries;

    void add(std::string name, factory f, std::vector<std::string> structural = {}) {
        entries[std::move(name)] = {std::move(f), std::move(structural)};
    }
    bool has(const std::string& name) const { return entries.count(name) != 0; }
    const entry* find(const std::string& name) const {
        auto it = entries.find(name);
        return it == entries.end() ? nullptr : &it->second;
    }
    std::unique_ptr<node> make(const std::string& name, const node_args& args) const {
        const entry* e = find(name);
        return e ? e->make(args) : nullptr;
    }
};

// The built-in node vocabulary (spec §14). Names deliberately terse for the DSL.
inline node_registry standard_registry() {
    node_registry r;
    r.add("sine",     [](const node_args& a){ return std::make_unique<oscillator>(a.num("freq", 0, 440.0)); });
    r.add("noise",    [](const node_args&)  { return std::make_unique<white_noise>(); });
    r.add("constant", [](const node_args& a){ return std::make_unique<constant>(a.num("value", 0, 1.0)); });
    r.add("gain",     [](const node_args& a){ return std::make_unique<gain>(a.num("gain", 0, 1.0)); });
    r.add("fader",    [](const node_args& a){ return std::make_unique<stereo_fader>(a.num("volume", 0, 1.0)); });
    r.add("pan",      [](const node_args& a){ return std::make_unique<pan>(a.num("pan", 0, 0.0)); });
    r.add("mix",      [](const node_args& a){ return std::make_unique<mixer>(a.count("inputs", 0, 2)); }, {"inputs"});
    r.add("smix",     [](const node_args& a){ return std::make_unique<stereo_mixer>(a.count("inputs", 0, 2)); }, {"inputs"});
    r.add("adapt",    [](const node_args& a){ return std::make_unique<channel_adapter>(a.count("inputs", 0, 1), a.count("outputs", 1, 1)); }, {"inputs", "outputs"});
    r.add("m2s",      [](const node_args&)  { return std::make_unique<mono_to_stereo>(); });
    r.add("s2m",      [](const node_args&)  { return std::make_unique<stereo_to_mono>(); });
    r.add("inv",      [](const node_args&)  { return std::make_unique<inverter>(); });
    r.add("add",      [](const node_args&)  { return std::make_unique<add>(); });
    r.add("mul",      [](const node_args&)  { return std::make_unique<multiply>(); });
    r.add("sah",      [](const node_args& a){ return std::make_unique<sample_and_hold>(a.num("rate", 0, 1.0)); });
    r.add("bus",      [](const node_args& a){ return std::make_unique<bus_input>(a.count("channels", 0, 2)); }, {"channels"});
    r.add("up2",      [](const node_args&)  { return std::make_unique<upsampler2>(); });
    r.add("down2",    [](const node_args&)  { return std::make_unique<downsampler2>(); });
    r.add("downbloc", [](const node_args&)  { return std::make_unique<downbloc>(); });
    r.add("stft",     [](const node_args& a){ return std::make_unique<ola_stft>(a.count("size", 0, 1024)); }, {"size"});
    r.add("stft_fwd", [](const node_args& a){ return std::make_unique<stft_forward>(a.count("size", 0, 1024), a.count("channels", 1, 1)); }, {"size", "channels"});
    r.add("stft_bwd", [](const node_args& a){ return std::make_unique<stft_backward>(a.count("size", 0, 1024), a.count("channels", 1, 1)); }, {"size", "channels"});
    return r;
}

} // namespace xrune::galdr
