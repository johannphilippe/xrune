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

#pragma once
#include "xrune/blueprint.hpp"
#include "xrune/lang/node_registry.hpp"
#include "xrune/lang/parser/ast.hpp"
#include "xrune/lang/parser/diagnostic.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <stdexcept>

// Xrune lang semantic analysis + lowering: AST -> graph_blueprint. Each expression
// evaluates to a `value`, either a compile-time number or a `fragment` (a
// sub-graph with open input ports and output channels, à la Faust block
// diagrams). The connection algebra composes fragments with strict arity;
// sigils/rune-params expand at compile time. Output is a graph_blueprint per
// rune, ready for runtime::register_blueprint.

namespace xrune::lang {

// An output channel or an input port of some node in the blueprint.
struct chan { size_t node = 0; size_t ch = 0; };

// A block: ordered open input ports + ordered output channels.
struct fragment {
    std::vector<chan> ins;
    std::vector<chan> outs;
};

struct value {
    bool is_number = false;
    double number = 0.0;
    fragment frag;
    static value num(double n) { value v; v.is_number = true; v.number = n; return v; }
    static value block(fragment f) { value v; v.is_number = false; v.frag = std::move(f); return v; }
};

struct lowered_blueprint {
    std::string name;
    graph_blueprint bp;
};

struct lower_result {
    std::vector<lowered_blueprint> blueprints;
    diagnostics diags;
    bool ok() const { return diags.empty(); }
};

using env_t = std::unordered_map<std::string, value>;

struct lang_abort {}; // internal: abort the current rune

struct lowerer {
    const node_registry& reg;
    std::unordered_map<std::string, const sigil_def*> sigils;
    diagnostics& diags;

    graph_blueprint* bp = nullptr; // current blueprint

    // Naming scope: nodes created inside a bound fragment / sigil expansion are
    // prefixed with the call-site binding path (spec §10/§11), so Idyl can
    // address them by stable dotted names (e.g. "bass.sine#1", "v1.osc").
    std::string prefix;
    std::unordered_map<std::string, int> counters; // per-prefix anonymous counter

    // Rune-parameter tracking, for promoting them to blueprint parameters.
    std::unordered_set<std::string> rune_params;
    std::unordered_map<std::string, size_t> uses;         // every evaluation
    std::unordered_map<std::string, size_t> direct_uses;  // bound to a port

    lowerer(const node_registry& r, diagnostics& d) : reg(r), diags(d) {}

    [[noreturn]] void fail(int line, int col, const std::string& m) {
        diags.push_back({m, line, col});
        throw lang_abort{};
    }

    std::string qualify(const std::string& n) const { return prefix.empty() ? n : prefix + "." + n; }
    std::string gen(const std::string& type) { return qualify(type + "#" + std::to_string(++counters[prefix])); }

    size_t add(std::unique_ptr<node> n, const std::string& name) {
        size_t idx = bp->nodes.size();
        bp->nodes.push_back(std::move(n));
        bp->names.push_back(name);
        return idx;
    }

    fragment node_fragment(size_t idx) {
        node* n = bp->nodes[idx].get();
        fragment f;
        for (size_t i = 0; i < n->inputs_count(); ++i) f.ins.push_back({idx, i});
        for (size_t o = 0; o < n->outputs_count(); ++o) f.outs.push_back({idx, o});
        return f;
    }

    void wire(chan out, chan in, int line, int col) {
        if (!bp->connect(out.node, out.ch, in.node, in.ch))
            fail(line, col, "connection rejected: input already driven or channel out of range");
    }

    // ---- builtin node makers (signal arithmetic, `_`, `!`) ----

    size_t make_wire()  { return add(reg.make("adapt", one_one()), gen("wire")); }
    size_t make_cut()   { return add(reg.make("cut", {}), gen("cut")); }
    size_t make_const(double v) { return add(reg.make("constant", named("value", v)), gen("const")); }
    size_t make_gain(double v)  { return add(reg.make("gain", named("gain", v)), gen("gain")); }
    size_t make_op(const char* type) { return add(reg.make(type, {}), gen(type)); }

    static node_args named(const char* k, double v) {
        node_args a; a.named.emplace_back(k, arg_value(v)); return a;
    }
    static node_args one_one() {
        node_args a;
        a.named.emplace_back("inputs", arg_value(1.0));
        a.named.emplace_back("outputs", arg_value(1.0));
        return a;
    }

    // sig * k  ->  one gain(k) per channel. Deliberately NOT mul+constant: one
    // node instead of two, and the `gain` port stays addressable, so a literal
    // scale factor remains automatable/modulatable later.
    fragment scale(fragment a, double k, const expr& e, const expr* scalar = nullptr) {
        // If the scale factor is *exactly* a rune parameter (`osc * amp`), bind it
        // to the gain port this creates. Without this, the most natural way to
        // write a level control would silently produce an unaddressable node --
        // the node-call binder never sees it, because arithmetic builds the gain
        // itself rather than going through eval_call().
        const bool bindable = scalar && scalar->k == expr::kind::ident &&
                              rune_params.count(scalar->text) &&
                              bp->find_rune_param(scalar->text) >= 0;

        fragment out; out.ins = a.ins;
        for (chan c : a.outs) {
            size_t g = make_gain(k);
            wire(c, {g, 0}, e.line, e.col);
            out.outs.push_back({g, 0});
            if (bindable) {
                const long pi = bp->find_rune_param(scalar->text);
                bp->params[static_cast<size_t>(pi)].targets.push_back({g, 0});
                ++direct_uses[scalar->text];
            }
        }
        return out;
    }

    // Element-wise binary op across channels, broadcasting a mono operand.
    // (This is where Xrune diverges from Faust, which would require the operand
    // arities to feed a 2-input primitive exactly. Broadcasting makes stereo
    // work without ceremony; see dev/language_improvements.md.)
    fragment zip(const char* type, fragment a, fragment b, const expr& e) {
        const size_t na = a.outs.size(), nb = b.outs.size();
        if (na != nb && na != 1 && nb != 1)
            fail(e.line, e.col, "arithmetic arity: cannot combine " + std::to_string(na) +
                                " and " + std::to_string(nb) + " channels");
        const size_t n = (na > nb) ? na : nb;
        fragment out;
        out.ins = a.ins;
        out.ins.insert(out.ins.end(), b.ins.begin(), b.ins.end());
        for (size_t i = 0; i < n; ++i) {
            size_t op = make_op(type);
            wire(a.outs[na == 1 ? 0 : i], {op, 0}, e.line, e.col);
            wire(b.outs[nb == 1 ? 0 : i], {op, 1}, e.line, e.col);
            out.outs.push_back({op, 0});
        }
        return out;
    }

    // -sig  ->  inv per channel (used by unary minus and by `a - b`).
    fragment negate(fragment a, const expr& e) {
        fragment out; out.ins = a.ins;
        for (chan c : a.outs) {
            size_t v = make_op("inv");
            wire(c, {v, 0}, e.line, e.col);
            out.outs.push_back({v, 0});
        }
        return out;
    }

    // ---- lowering entry ----
    void lower_rune(const rune_def& r) {
        prefix.clear();
        counters.clear();
        env_t env;
        std::unordered_set<std::string> defined;

        rune_params.clear();
        uses.clear();
        direct_uses.clear();

        for (const auto& p : r.params) {
            if (!p.default_value)
                fail(p.line, p.col, "rune parameter '" + p.name + "' needs a default value");
            const double dv = eval_number(*p.default_value, env);
            env[p.name] = value::num(dv);
            defined.insert(p.name);

            rune_params.insert(p.name);
            blueprint_param bp_param;
            bp_param.name = p.name;
            bp_param.default_value = dv;
            bp->params.push_back(std::move(bp_param));
        }

        bool have_out = false;
        for (const auto& s : r.body) do_stmt(*s, env, defined, &have_out);

        // A parameter used more times than it was bound directly to a port has
        // at least one use that got folded into a constant (`f * 2`). That use
        // cannot follow the parameter at run time, so flag it instead of letting
        // the host discover the inconsistency by ear.
        for (auto& bpp : bp->params) {
            const size_t total  = uses.count(bpp.name)        ? uses[bpp.name]        : 0;
            const size_t direct = direct_uses.count(bpp.name) ? direct_uses[bpp.name] : 0;
            bpp.partial = (total > direct);
        }
        if (!have_out) fail(r.line, r.col, "rune '" + r.name + "' has no 'out'");
    }

    // A statement inside a rune body (have_out non-null) or a sigil body (null).
    // Returns the value of a bare wire expression (used for a sigil's last-value).
    value do_stmt(const stmt& s, env_t& env, std::unordered_set<std::string>& defined, bool* have_out) {
        switch (s.k) {
            case stmt::kind::binding: {
                if (defined.count(s.name)) fail(s.line, s.col, "duplicate name '" + s.name + "'");
                const std::string qname = qualify(s.name);
                // A single *node* call bound to x is named exactly x (qualified).
                // Everything else (a sigil call, a builtin, or a composite
                // expression) opens a naming scope: nodes become x.type#k and
                // inner named bindings become x.inner.
                const bool single_node = s.value->k == expr::kind::call &&
                                         s.value->text != "over" && s.value->text != "finer" &&
                                         !sigils.count(s.value->text) && reg.has(s.value->text);
                value v;
                if (single_node) {
                    v = eval_call(*s.value, env, qname);
                } else {
                    std::string saved = prefix;
                    prefix = qname;
                    v = eval_expr(*s.value, env);
                    prefix = saved;
                }
                defined.insert(s.name);
                env[s.name] = std::move(v);
                return value::num(0);
            }
            case stmt::kind::wire:
                return eval_expr(*s.expr_, env);
            case stmt::kind::modulate: do_modulate(s, env); return value::num(0);
            case stmt::kind::explicit_wire: do_explicit(s, env); return value::num(0);
            case stmt::kind::input:
                if (!have_out) fail(s.line, s.col, "'input' is not allowed inside a sigil");
                do_input(s, env, defined); return value::num(0);
            case stmt::kind::out:
                if (!have_out) fail(s.line, s.col, "'out' is not allowed inside a sigil");
                do_out(s, env, *have_out); return value::num(0);
        }
        return value::num(0);
    }

    // ---- statements ----
    void do_input(const stmt& s, env_t& env, std::unordered_set<std::string>& defined) {
        if (defined.count(s.name)) fail(s.line, s.col, "duplicate name '" + s.name + "'");
        node_args na = eval_node_args(s.args, env);
        std::unique_ptr<node> nd = reg.make("bus", na);
        size_t idx = add(std::move(nd), s.name);
        bp->add_input_terminal(s.name, idx);
        defined.insert(s.name);
        env[s.name] = value::block(node_fragment(idx));
    }

    void do_out(const stmt& s, env_t& env, bool& have_out) {
        fragment f = eval_fragment(*s.expr_, env);
        if (f.outs.empty()) fail(s.line, s.col, "'out' expression produces no output");
        size_t node0 = f.outs[0].node;
        for (const auto& o : f.outs)
            if (o.node != node0)
                fail(s.line, s.col, "'out' must be a single node (its channels come from more than "
                                    "one node); sum them with a mixer/fader first");
        if (s.terminal.empty()) {
            if (have_out) fail(s.line, s.col, "only one plain 'out' allowed; use 'out X as name' for extra terminals");
            bp->set_output(node0);
            have_out = true;
        } else {
            if (s.terminal == "out") fail(s.line, s.col, "'as out' is reserved; use a plain 'out'");
            bp->add_output_terminal(s.terminal, node0);
        }
    }

    void do_modulate(const stmt& s, env_t& env) {
        fragment src = eval_fragment(*s.source, env);
        if (src.outs.size() != 1) fail(s.line, s.col, "modulation source must have exactly one output");
        const auto& parts = s.target.parts;
        std::string node_name = parts[0];
        for (size_t i = 1; i + 1 < parts.size(); ++i) node_name += "." + parts[i];
        const std::string& port = parts.back();
        long ni = bp->find_node(node_name);
        if (ni < 0) fail(s.target.line, s.target.col, "modulation target: unknown node '" + node_name + "'");
        long pi = bp->find_param(static_cast<size_t>(ni), port);
        if (pi < 0) fail(s.target.line, s.target.col,
                         "modulation target: node '" + node_name + "' has no port '" + port + "'");
        if (!bp->connect_param(src.outs[0].node, src.outs[0].ch, static_cast<size_t>(ni), static_cast<size_t>(pi)))
            fail(s.line, s.col, "modulation rejected: port already driven or out of range");
    }

    void do_explicit(const stmt& s, env_t& env) {
        fragment src = eval_fragment(*s.source, env);
        if (src.outs.size() != 1) fail(s.line, s.col, "'->' source must have exactly one output");
        long ni = bp->find_node(s.target_node);
        if (ni < 0) fail(s.line, s.col, "'->' target: unknown node '" + s.target_node + "'");
        wire(src.outs[0], {static_cast<size_t>(ni), static_cast<size_t>(s.target_input)}, s.line, s.col);
    }

    // ---- expressions ----
    value eval_expr(const expr& e, env_t& env, const std::string& preferred = "") {
        switch (e.k) {
            case expr::kind::number: return value::num(e.number);
            case expr::kind::string:
            case expr::kind::boolean:
                fail(e.line, e.col, "a string/boolean is only valid as a node argument");
            case expr::kind::ident: {
                auto it = env.find(e.text);
                if (it != env.end()) {
                    if (rune_params.count(e.text)) ++uses[e.text];
                    return it->second;
                }
                // `_` is the identity wire and `!` discards a channel. Both are
                // lowered to a node (adapt(1,1) / cut) so they behave correctly
                // in EVERY position — including `_ , _ : add`, where a bare wire
                // has an open input that the {node,channel} fragment model can
                // only express as a node. The redundant adapt(1,1) is a peephole
                // candidate for compile(), not a semantic problem.
                if (e.text == "_") return value::block(node_fragment(make_wire()));
                if (e.text == "!") return value::block(node_fragment(make_cut()));
                // A bare name that is a registered node type instantiates with
                // defaults (Faust-style: `a : m2s`, `x :> add`).
                if (reg.has(e.text)) {
                    size_t idx = add(reg.make(e.text, {}), gen(e.text));
                    return value::block(node_fragment(idx));
                }
                fail(e.line, e.col, "unknown name '" + e.text + "'");
            }
            case expr::kind::unary: {
                value v = eval_expr(*e.a, env);
                if (v.is_number) return value::num(-v.number);
                return value::block(negate(std::move(v.frag), e));   // -sig -> inv
            }
            case expr::kind::select: {
                fragment f = eval_fragment(*e.a, env);
                if (static_cast<size_t>(e.index) >= f.outs.size())
                    fail(e.line, e.col, "channel [" + std::to_string(e.index) + "] out of range (fragment has "
                                            + std::to_string(f.outs.size()) + " outputs)");
                fragment r; r.ins = f.ins; r.outs = {f.outs[static_cast<size_t>(e.index)]};
                return value::block(std::move(r));
            }
            case expr::kind::binary: return eval_binary(e, env);
            case expr::kind::call: return eval_call(e, env, preferred);
        }
        return value::num(0);
    }

    // Arithmetic. The OPERATOR picks the operation; the KINDS of the operands
    // pick the lowering:
    //
    //   num . num   compile-time fold (unchanged)
    //   sig * num   gain(num) per channel          num * sig  same
    //   sig / num   gain(1/num)  (reciprocal folded at compile time)
    //   sig + num   add + constant per channel     num + sig  same
    //   sig - num   add + constant(-num)           num - sig  constant + inv
    //   sig . sig   element-wise per channel, mono operand broadcast
    //
    // `%` stays compile-time only: there is no modulo node, and a per-sample
    // modulo is not something anyone has asked the audio graph for.
    value eval_arith(const expr& e, env_t& env) {
        value l = eval_expr(*e.a, env);
        value r = eval_expr(*e.b, env);

        // Both numbers: fold, exactly as before.
        if (l.is_number && r.is_number) {
            const double a = l.number, b = r.number;
            switch (e.op) {
                case Tok::Plus:  return value::num(a + b);
                case Tok::Minus: return value::num(a - b);
                case Tok::Star:  return value::num(a * b);
                case Tok::Slash:
                    if (b == 0.0) fail(e.line, e.col, "division by zero");
                    return value::num(a / b);
                case Tok::Percent:
                    if (b == 0.0) fail(e.line, e.col, "modulo by zero");
                    return value::num(std::fmod(a, b));
                default: break;
            }
            return value::num(0);
        }

        if (e.op == Tok::Percent)
            fail(e.line, e.col, "'%' is compile-time only; it does not apply to signals");

        // At least one side is a signal.
        switch (e.op) {
            case Tok::Star:
                if (r.is_number) return value::block(scale(std::move(l.frag), r.number, e, e.b.get()));
                if (l.is_number) return value::block(scale(std::move(r.frag), l.number, e, e.a.get()));
                return value::block(zip("mul", std::move(l.frag), std::move(r.frag), e));

            case Tok::Slash:
                if (r.is_number) {
                    if (r.number == 0.0) fail(e.line, e.col, "division by zero");
                    return value::block(scale(std::move(l.frag), 1.0 / r.number, e));  // reciprocal
                }
                // num / sig, or sig / sig -> the div node (which yields 0 on /0).
                return value::block(zip("div", to_frag(std::move(l), e), std::move(r.frag), e));

            case Tok::Plus:
                return value::block(zip("add", to_frag(std::move(l), e), to_frag(std::move(r), e), e));

            case Tok::Minus: {
                // a - b  ==  a + (-b). No `sub` node needed: `inv` already exists.
                fragment a = to_frag(std::move(l), e);
                if (r.is_number) {
                    fragment k = node_fragment(make_const(-r.number));
                    return value::block(zip("add", std::move(a), std::move(k), e));
                }
                fragment b = negate(std::move(r.frag), e);
                return value::block(zip("add", std::move(a), std::move(b), e));
            }
            default: break;
        }
        fail(e.line, e.col, "unsupported operator");
        return value::num(0);
    }

    // A value in signal position: a number becomes a constant (DC) signal.
    fragment to_frag(value v, const expr& e) {
        (void)e;
        if (v.is_number) return node_fragment(make_const(v.number));
        return std::move(v.frag);
    }

    value eval_binary(const expr& e, env_t& env) {
        switch (e.op) {
            case Tok::Plus: case Tok::Minus: case Tok::Star:
            case Tok::Slash: case Tok::Percent:
                return eval_arith(e, env);
            case Tok::Colon: return eval_seq(e, env);
            case Tok::Comma: return eval_par(e, env);
            case Tok::Split: return eval_split(e, env);
            case Tok::Merge: return eval_merge(e, env);
            default:
                fail(e.line, e.col, "unsupported operator");
        }
    }

    value eval_seq(const expr& e, env_t& env) {
        fragment a = eval_fragment(*e.a, env);
        fragment b = eval_fragment(*e.b, env);
        if (a.outs.size() != b.ins.size())
            fail(e.line, e.col, "':' arity: left produces " + std::to_string(a.outs.size()) +
                                    " outputs but right takes " + std::to_string(b.ins.size()) + " inputs");
        for (size_t i = 0; i < a.outs.size(); ++i) wire(a.outs[i], b.ins[i], e.line, e.col);
        fragment r; r.ins = a.ins; r.outs = b.outs;
        return value::block(std::move(r));
    }

    value eval_par(const expr& e, env_t& env) {
        fragment a = eval_fragment(*e.a, env);
        fragment b = eval_fragment(*e.b, env);
        fragment r;
        r.ins = a.ins;  r.ins.insert(r.ins.end(), b.ins.begin(), b.ins.end());
        r.outs = a.outs; r.outs.insert(r.outs.end(), b.outs.begin(), b.outs.end());
        return value::block(std::move(r));
    }

    value eval_split(const expr& e, env_t& env) {
        fragment a = eval_fragment(*e.a, env);
        fragment b = eval_fragment(*e.b, env);
        size_t n = a.outs.size(), m = b.ins.size();
        if (n == 0 || (m % n) != 0)
            fail(e.line, e.col, "'<:' arity: right inputs (" + std::to_string(m) +
                                    ") must be a multiple of left outputs (" + std::to_string(n) + ")");
        for (size_t j = 0; j < m; ++j) wire(a.outs[j % n], b.ins[j], e.line, e.col);
        fragment r; r.ins = a.ins; r.outs = b.outs;
        return value::block(std::move(r));
    }

    value eval_merge(const expr& e, env_t& env) {
        fragment a = eval_fragment(*e.a, env);
        fragment b = eval_fragment(*e.b, env);
        size_t n = a.outs.size(), m = b.ins.size();
        if (m == 0 || (n % m) != 0)
            fail(e.line, e.col, "':>' arity: left outputs (" + std::to_string(n) +
                                    ") must be a multiple of right inputs (" + std::to_string(m) + ")");
        for (size_t i = 0; i < m; ++i) {
            std::vector<chan> group;
            for (size_t j = i; j < n; j += m) group.push_back(a.outs[j]);
            if (group.size() == 1) {
                wire(group[0], b.ins[i], e.line, e.col);
            } else {
                node_args ma; ma.named.push_back({"inputs", arg_value(static_cast<double>(group.size()))});
                size_t mix = add(reg.make("mix", ma), gen("mix"));
                for (size_t g = 0; g < group.size(); ++g) wire(group[g], {mix, g}, e.line, e.col);
                wire({mix, 0}, b.ins[i], e.line, e.col);
            }
        }
        fragment r; r.ins = a.ins; r.outs = b.outs;
        return value::block(std::move(r));
    }

    value eval_call(const expr& e, env_t& env, const std::string& preferred) {
        const std::string& name = e.text;
        if (name == "over") return eval_over(e, env);
        if (name == "finer")
            fail(e.line, e.col, "'finer' is not yet implemented (needs the upbloc Tier-2 node)");
        if (sigils.count(name)) return eval_sigil(e, env);
        if (reg.has(name)) {
            node_args na = eval_node_args(e.args, env);
            // A factory may throw -- faustlib() compiles Faust source, and that
            // can fail on an unknown function or a missing compile-time argument.
            // Turn it into a diagnostic WITH a source position, rather than an
            // exception escaping the compiler.
            std::unique_ptr<node> nd;
            try {
                nd = reg.make(name, na);
            } catch (const lang_abort&) {
                throw;
            } catch (const std::exception& ex) {
                fail(e.line, e.col, std::string("'") + name + "': " + ex.what());
            }
            if (!nd) fail(e.line, e.col, "'" + name + "': factory returned nothing");
            std::string nm = preferred.empty() ? gen(name) : preferred;
            size_t idx = add(std::move(nd), nm);
            bind_rune_params(e.args, idx);
            return value::block(node_fragment(idx));
        }
        fail(e.line, e.col, "unknown node type or sigil '" + name + "'");
    }

    value eval_over(const expr& e, env_t& env) {
        if (e.args.size() != 2) fail(e.line, e.col, "over(n, region) takes exactly two arguments");
        double nv = eval_number(*e.args[0].value, env);
        int n = static_cast<int>(std::llround(nv));
        if (n < 1 || (n & (n - 1)) != 0) fail(e.line, e.col, "over: n must be a power of two");
        fragment f = eval_fragment(*e.args[1].value, env);
        if (n == 1) return value::block(std::move(f));
        if (f.ins.size() != 1 || f.outs.size() != 1)
            fail(e.line, e.col, "over: the region must be 1-in / 1-out");
        int stages = 0; while ((1 << stages) < n) ++stages;

        size_t region_in = 0; chan cur{};
        for (int s = 0; s < stages; ++s) {
            size_t up = add(reg.make("up2", {}), gen("up2"));
            if (s == 0) region_in = up; else wire(cur, {up, 0}, e.line, e.col);
            cur = {up, 0};
        }
        wire(cur, f.ins[0], e.line, e.col);
        cur = f.outs[0];
        for (int s = 0; s < stages; ++s) {
            size_t dn = add(reg.make("down2", {}), gen("down2"));
            wire(cur, {dn, 0}, e.line, e.col);
            cur = {dn, 0};
        }
        fragment r; r.ins = {{region_in, 0}}; r.outs = {cur};
        return value::block(std::move(r));
    }

    value eval_sigil(const expr& e, env_t& env) {
        const sigil_def& sig = *sigils.at(e.text);
        env_t local;
        std::unordered_set<std::string> defined;
        // Bind parameters (named wins, else positional, else default).
        for (size_t i = 0; i < sig.params.size(); ++i) {
            const param& p = sig.params[i];
            const expr* provided = nullptr;
            for (const auto& a : e.args) if (a.name == p.name) { provided = a.value.get(); break; }
            if (!provided) {
                size_t pos = 0, seen = 0;
                for (const auto& a : e.args) { if (a.name.empty()) { if (seen == i) { provided = a.value.get(); break; } ++seen; } (void)pos; }
            }
            if (!provided) provided = p.default_value.get();
            if (!provided) fail(e.line, e.col, "sigil '" + e.text + "' missing argument '" + p.name + "'");
            local[p.name] = eval_expr(*provided, env); // evaluated in the caller's scope
            defined.insert(p.name);
        }
        value last = value::num(0);
        bool any = false;
        for (const auto& s : sig.body) { last = do_stmt(*s, local, defined, nullptr); any = (s->k == stmt::kind::wire); }
        if (!any) fail(sig.line, sig.col, "sigil '" + e.text + "' must end with an expression");
        return last;
    }

    // ---- helpers ----
    // A number used where a signal is expected becomes a constant (DC) signal.
    // This is unambiguous because the number/signal distinction is POSITIONAL and
    // already resolved: argument slots go through eval_number(), signal slots
    // through here. Mistakes are still caught -- `sine : 0.5` coerces 0.5 to a
    // `constant`, which has zero inputs, so ':' then fails on arity.
    fragment eval_fragment(const expr& e, env_t& env) {
        value v = eval_expr(e, env);
        if (v.is_number) return node_fragment(make_const(v.number));
        return std::move(v.frag);
    }
    double eval_number(const expr& e, env_t& env) {
        value v = eval_expr(e, env);
        if (!v.is_number) fail(e.line, e.col, "expected a number here, got a signal");
        return v.number;
    }

    // An argument that is EXACTLY a rune parameter (`sine(freq = f)`, `gain(amp)`)
    // binds that parameter to the port it fed, so the host can drive it later.
    // Anything else (`f * 2`) is folded to a number and cannot be followed.
    void bind_rune_params(const std::vector<argument>& args, size_t node_idx) {
        if (bp->params.empty()) return;
        const node* nd = bp->nodes[node_idx].get();

        // A node with structural arguments (mixer inputs, fft size) has
        // positional slots that are NOT ports, so positional binding is only
        // safe when it reports none.
        node_config_arg cfg[8];
        const bool positional_is_port = (nd->config_args(cfg, 8) == 0);

        size_t pos = 0;
        for (const auto& a : args) {
            const bool is_named = !a.name.empty();
            const size_t this_pos = is_named ? 0 : pos++;
            if (!a.value || a.value->k != expr::kind::ident) continue;
            const std::string& pname = a.value->text;
            const long pi_bp = bp->find_rune_param(pname);
            if (pi_bp < 0) continue;                       // not a rune parameter

            long port = -1;
            if (is_named) {
                port = bp->find_param(node_idx, a.name);   // freq = f
            } else if (positional_is_port && this_pos < nd->params_count()) {
                port = static_cast<long>(this_pos);        // gain(amp)
            }
            if (port < 0) continue;

            bp->params[static_cast<size_t>(pi_bp)].targets.push_back(
                {node_idx, static_cast<size_t>(port)});
            ++direct_uses[pname];
        }
    }

    node_args eval_node_args(const std::vector<argument>& args, env_t& env) {
        node_args na;
        for (const auto& a : args) {
            arg_value av;
            if (a.value->k == expr::kind::string) av = arg_value::make_string(a.value->text);
            else if (a.value->k == expr::kind::boolean) av = arg_value::make_bool(a.value->boolean);
            else av = arg_value(eval_number(*a.value, env));
            if (a.name.empty()) na.positional.push_back(av);
            else na.named.push_back({a.name, av});
        }
        return na;
    }
};

inline lower_result lower(const program& prog, const node_registry& reg) {
    lower_result res;
    lowerer L(reg, res.diags);
    for (const auto& s : prog.sigils) L.sigils[s.name] = &s;
    for (const auto& r : prog.runes) {
        lowered_blueprint lb;
        lb.name = r.name;
        L.bp = &lb.bp;
        try {
            L.lower_rune(r);
            res.blueprints.push_back(std::move(lb));
        } catch (const lang_abort&) {
            // diagnostic already recorded; skip this rune
        }
    }
    return res;
}

} // namespace xrune::lang
