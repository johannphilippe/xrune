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

#include <cstdio>
#include <memory>
#include <vector>

namespace xrune {




// ---------------------------------------------------------------------------
// JSON export
// ---------------------------------------------------------------------------

json::value to_json_value(const graph_blueprint& bp, const std::string& name) {
    json::value root = json::value::make_object();
    root.set("xrune", json::value(double(serialize_format_version)));
    if (!name.empty()) root.set("name", json::value(name));

    json::value nodes = json::value::make_array();
    for (size_t i = 0; i < bp.nodes.size(); ++i) {
        const node* n = bp.nodes[i].get();
        json::value jn = json::value::make_object();
        jn.set("id", json::value(double(i)));
        jn.set("type", json::value(n->type_name()));
        if (i < bp.names.size() && !bp.names[i].empty())
            jn.set("name", json::value(bp.names[i]));

        // args = constructor config (non-port) + every port's effective default.
        // Both are keyed the way the registry factories read them, so a factory
        // rebuilds the node exactly.
        json::value args = json::value::make_object();
        node_config_arg cfg[8];
        size_t ncfg = n->config_args(cfg, 8);
        for (size_t c = 0; c < ncfg; ++c)
            args.set(cfg[c].name, json::value(double(cfg[c].value)));

        const port_descriptor* pd = n->params();
        for (size_t p = 0; p < n->params_count(); ++p)
            args.set(pd[p].name, json::value(double(n->param_default(p))));

        if (!args.obj.empty()) jn.set("args", std::move(args));
        nodes.push(std::move(jn));
    }
    root.set("nodes", std::move(nodes));

    json::value conns = json::value::make_array();
    for (const auto& c : bp.connections) {
        json::value jc = json::value::make_object();
        jc.set("src", json::value(double(c.src_node)));
        jc.set("out", json::value(double(c.src_output)));
        jc.set("dst", json::value(double(c.dst_node)));
        jc.set("in",  json::value(double(c.dst_input)));
        conns.push(std::move(jc));
    }
    root.set("connections", std::move(conns));

    json::value mods = json::value::make_array();
    for (const auto& m : bp.param_connections) {
        json::value jm = json::value::make_object();
        jm.set("src", json::value(double(m.src_node)));
        jm.set("out", json::value(double(m.src_output)));
        jm.set("dst", json::value(double(m.dst_node)));
        jm.set("param", json::value(double(m.dst_param)));
        mods.push(std::move(jm));
    }
    root.set("modulations", std::move(mods));

    auto terminals = [](const std::vector<terminal>& ts) {
        json::value a = json::value::make_array();
        for (const auto& t : ts) {
            json::value jt = json::value::make_object();
            jt.set("name", json::value(t.name));
            jt.set("node", json::value(double(t.node)));
            if (!t.channels.empty()) {   // output terminals: per-channel sources
                json::value chans = json::value::make_array();
                for (const terminal_source& c : t.channels) {
                    json::value jc = json::value::make_object();
                    if (c.silent) {
                        jc.set("silent", json::value(true));
                    } else {
                        jc.set("node", json::value(double(c.node)));
                        jc.set("ch", json::value(double(c.ch)));
                    }
                    chans.push(std::move(jc));
                }
                jt.set("channels", std::move(chans));
            }
            a.push(std::move(jt));
        }
        return a;
    };
    root.set("input_terminals",  terminals(bp.input_terminals));
    root.set("output_terminals", terminals(bp.output_terminals));
    root.set("input_node",  json::value(double(bp.input_node)));
    root.set("output_node", json::value(double(bp.output_node)));
    return root;
}

std::string to_json(const graph_blueprint& bp, const std::string& name, int indent) {
    return json::dump(to_json_value(bp, name), indent);
}

// ---------------------------------------------------------------------------
// JSON import
// ---------------------------------------------------------------------------

// Rebuilds `out` from JSON text. Nodes are constructed via `reg`, so a type that
// isn't registered is a hard error (reported, not silently dropped). Returns
// false and fills `err` on any problem; `out` is then left incomplete.
bool from_json(const std::string& text,
                      const lang::node_registry& reg,
                      graph_blueprint& out,
                      std::string& err,
                      std::string* name_out) {
    json::value root;
    if (!json::parse(text, root, err)) return false;
    if (!root.is_object()) { err = "top level must be an object"; return false; }

    const long ver = root.integer("xrune", 0);
    if (ver != serialize_format_version) {
        err = "unsupported format version " + std::to_string(ver) +
              " (expected " + std::to_string(serialize_format_version) + ")";
        return false;
    }
    if (name_out) *name_out = root.text("name");

    const json::value* jnodes = root.find("nodes");
    if (!jnodes || !jnodes->is_array()) { err = "missing 'nodes' array"; return false; }

    out = graph_blueprint{};

    for (const json::value& jn : jnodes->arr) {
        if (!jn.is_object()) { err = "node entry must be an object"; return false; }
        const std::string type = jn.text("type");
        if (type.empty()) { err = "node is missing 'type'"; return false; }
        if (!reg.has(type)) { err = "unknown node type: '" + type + "'"; return false; }

        // Every arg becomes a *named* argument; the factories resolve named
        // first, so this reconstructs both structural config and port defaults.
        lang::node_args args;
        if (const json::value* ja = jn.find("args"); ja && ja->is_object()) {
            for (const auto& kv : ja->obj) {
                const json::value& v = kv.second;
                lang::arg_value av;
                if (v.is_number())      av = lang::arg_value(v.number);
                else if (v.is_string()) av = lang::arg_value::make_string(v.str);
                else if (v.k == json::value::kind::boolean)
                                        av = lang::arg_value::make_bool(v.boolean);
                else continue;          // null / nested: not an argument
                args.named.emplace_back(kv.first, std::move(av));
            }
        }

        std::unique_ptr<node> n = reg.make(type, args);
        if (!n) { err = "factory for '" + type + "' returned null"; return false; }
        out.nodes.push_back(std::move(n));
        out.names.push_back(jn.text("name"));   // "" = unnamed; stays parallel
    }

    const size_t count = out.nodes.size();
    auto valid = [&](long idx) { return idx >= 0 && static_cast<size_t>(idx) < count; };

    if (const json::value* jc = root.find("connections"); jc && jc->is_array()) {
        for (const json::value& c : jc->arr) {
            const long src = c.integer("src", -1), dst = c.integer("dst", -1);
            const long o = c.integer("out", 0), i = c.integer("in", 0);
            if (!valid(src) || !valid(dst)) { err = "connection references an unknown node"; return false; }
            // Route through connect() so port bounds and single-source-per-input
            // are enforced on load exactly as they are when building in C++.
            if (!out.connect(size_t(src), size_t(o), size_t(dst), size_t(i))) {
                err = "invalid connection " + std::to_string(src) + ":" + std::to_string(o) +
                      " -> " + std::to_string(dst) + ":" + std::to_string(i);
                return false;
            }
        }
    }

    if (const json::value* jm = root.find("modulations"); jm && jm->is_array()) {
        for (const json::value& m : jm->arr) {
            const long src = m.integer("src", -1), dst = m.integer("dst", -1);
            const long o = m.integer("out", 0), p = m.integer("param", 0);
            if (!valid(src) || !valid(dst)) { err = "modulation references an unknown node"; return false; }
            // connect_param() enforces port bounds and one source per port, the
            // same rules a C++-built blueprint gets.
            if (!out.connect_param(size_t(src), size_t(o), size_t(dst), size_t(p))) {
                err = "invalid modulation " + std::to_string(src) + ":" + std::to_string(o) +
                      " -> " + std::to_string(dst) + " param " + std::to_string(p);
                return false;
            }
        }
    }

    auto load_terminals = [&](const char* key, std::vector<terminal>& dst) -> bool {
        const json::value* jt = root.find(key);
        if (!jt || !jt->is_array()) return true;
        for (const json::value& t : jt->arr) {
            const long nd = t.integer("node", -1);
            if (!valid(nd)) { err = std::string(key) + " references an unknown node"; return false; }
            terminal term;
            term.name = t.text("name");
            term.node = size_t(nd);
            if (const json::value* ch = t.find("channels"); ch && ch->is_array()) {
                for (const json::value& c : ch->arr) {
                    if (const json::value* s = c.find("silent"); s && s->k == json::value::kind::boolean && s->boolean) {
                        term.channels.push_back({0, 0, true});
                        continue;
                    }
                    const long cn = c.integer("node", -1);
                    if (!valid(cn)) { err = std::string(key) + " channel references an unknown node"; return false; }
                    term.channels.push_back({size_t(cn), size_t(c.integer("ch", 0)), false});
                }
            }
            dst.push_back(std::move(term));
        }
        return true;
    };
    if (!load_terminals("input_terminals",  out.input_terminals))  return false;
    if (!load_terminals("output_terminals", out.output_terminals)) return false;

    // Set the fields directly: set_output() would append a second "out" terminal
    // on top of the ones we just restored.
    out.input_node  = root.integer("input_node",  -1);
    out.output_node = root.integer("output_node", -1);
    return true;
}

// ---------------------------------------------------------------------------
// Graphviz DOT export
// ---------------------------------------------------------------------------

namespace detail {

std::string dot_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

std::string trim_zeros(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

} // namespace detail

// Renders the blueprint as Graphviz DOT. Audio connections are solid edges;
// port modulation (an audio-rate signal driving a control port) is a dashed
// blue edge labelled with the port name. Terminals are drawn as distinct shapes
// so the instance's boundary is visible.
//
//   dot -Tsvg patch.dot -o patch.svg
std::string to_dot(const graph_blueprint& bp, const std::string& name) {
    std::string o;
    o += "digraph \"" + detail::dot_escape(name) + "\" {\n";
    o += "  rankdir=LR;\n";
    o += "  graph [fontname=\"Helvetica\", labelloc=\"t\", label=\"" + detail::dot_escape(name) + "\"];\n";
    o += "  node  [fontname=\"Helvetica\", shape=box, style=\"rounded,filled\", fillcolor=\"#eef3fa\", color=\"#4a6fa5\"];\n";
    o += "  edge  [fontname=\"Helvetica\", fontsize=9, color=\"#4a6fa5\"];\n\n";

    for (size_t i = 0; i < bp.nodes.size(); ++i) {
        const node* n = bp.nodes[i].get();
        const std::string label = (i < bp.names.size() && !bp.names[i].empty())
                                ? bp.names[i] : ("#" + std::to_string(i));

        // Label: name, type, then the ports with their defaults — the same
        // information the JSON carries, in a form you can read at a glance.
        std::string text = detail::dot_escape(label) + "\\n<" + n->type_name() + ">";
        const port_descriptor* pd = n->params();
        for (size_t p = 0; p < n->params_count(); ++p)
            text += "\\n" + std::string(pd[p].name) + " = " +
                    detail::trim_zeros(n->param_default(p));

        // A rate-changing node is a region boundary — worth seeing.
        if (n->rate_num() != n->rate_den() || n->block_num() != n->block_den()) {
            text += "\\nrate " + std::to_string(n->rate_num()) + "/" + std::to_string(n->rate_den());
            text += ", block " + std::to_string(n->block_num()) + "/" + std::to_string(n->block_den());
            o += "  n" + std::to_string(i) + " [label=\"" + text +
                 "\", fillcolor=\"#fdf0e3\", color=\"#c07b30\"];\n";
        } else {
            o += "  n" + std::to_string(i) + " [label=\"" + text + "\"];\n";
        }
    }

    o += "\n";
    for (const auto& c : bp.connections) {
        o += "  n" + std::to_string(c.src_node) + " -> n" + std::to_string(c.dst_node);
        // Only label the ports when they're not the trivial 0 -> 0 case.
        if (c.src_output != 0 || c.dst_input != 0)
            o += " [label=\"" + std::to_string(c.src_output) + ":" + std::to_string(c.dst_input) + "\"]";
        o += ";\n";
    }

    for (const auto& m : bp.param_connections) {
        const node* dst = bp.nodes[m.dst_node].get();
        const port_descriptor* pd = dst->params();
        const std::string port = (pd && m.dst_param < dst->params_count())
                               ? pd[m.dst_param].name : std::to_string(m.dst_param);
        o += "  n" + std::to_string(m.src_node) + " -> n" + std::to_string(m.dst_node) +
             " [style=dashed, color=\"#3b7d4f\", fontcolor=\"#3b7d4f\", label=\"" +
             detail::dot_escape(port) + "\"];\n";
    }

    if (!bp.input_terminals.empty() || !bp.output_terminals.empty()) o += "\n";
    for (size_t t = 0; t < bp.input_terminals.size(); ++t) {
        const auto& term = bp.input_terminals[t];
        o += "  in" + std::to_string(t) + " [label=\"" + detail::dot_escape(term.name) +
             "\", shape=invhouse, fillcolor=\"#e8f5e9\", color=\"#3b7d4f\"];\n";
        o += "  in" + std::to_string(t) + " -> n" + std::to_string(term.node) + " [style=bold];\n";
    }
    for (size_t t = 0; t < bp.output_terminals.size(); ++t) {
        const auto& term = bp.output_terminals[t];
        o += "  out" + std::to_string(t) + " [label=\"" + detail::dot_escape(term.name) +
             "\", shape=house, fillcolor=\"#e8f5e9\", color=\"#3b7d4f\"];\n";
        o += "  n" + std::to_string(term.node) + " -> out" + std::to_string(t) + " [style=bold];\n";
    }

    o += "}\n";
    return o;
}


} // namespace xrune
