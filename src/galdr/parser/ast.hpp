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
#include "token.hpp"
#include <string>
#include <vector>
#include <memory>

// Galdr abstract syntax tree — the parser's output. Purely syntactic: kind
// resolution (number vs node vs fragment), arity, and desugaring happen in
// later passes (sema/codegen), not here.

namespace xrune::galdr {

struct expr;
using expr_ptr = std::unique_ptr<expr>;

// One call/param argument: optional name (empty = positional) + value expr.
struct argument {
    std::string name;   // "" for positional
    expr_ptr value;
    int line = 0, col = 0;
};

struct expr {
    enum class kind {
        number,   // numeric literal
        string,   // string literal
        boolean,  // true/false
        ident,    // a binding / node reference / parameter reference
        call,     // type(args)  — a node instantiation or sigil/builtin call
        unary,    // -x
        binary,   // a <op> b   (: , <: :> + - * / %)
        select,   // expr[k]
    };

    kind k;
    int line = 0, col = 0;

    // number / boolean
    double number = 0.0;
    bool boolean = false;
    // string / ident / call callee
    std::string text;
    // unary / binary operator
    Tok op = Tok::Unknown;
    // unary operand / binary lhs,rhs / select base
    expr_ptr a, b;
    // call arguments
    std::vector<argument> args;
    // select index
    int index = 0;

    explicit expr(kind kk) : k(kk) {}
};

// A dotted port path on the right of `~>` : node.port or frag.node.port.
struct port_path {
    std::vector<std::string> parts; // >= 2
    int line = 0, col = 0;
};

struct stmt {
    enum class kind { binding, wire, modulate, explicit_wire, input, out };
    kind k;
    int line = 0, col = 0;

    // binding: name = value
    std::string name;
    expr_ptr value;

    // wire: bare expression (a connection with side effects)
    expr_ptr expr_;

    // modulate: source ~> target
    expr_ptr source;
    port_path target;

    // explicit_wire: source -> target_node[target_input]
    std::string target_node;
    int target_input = 0;

    // input: name(args)
    std::vector<argument> args;

    // out: expr [as terminal]
    std::string terminal; // "" -> "out"

    explicit stmt(kind kk) : k(kk) {}
};
using stmt_ptr = std::unique_ptr<stmt>;

struct param {
    std::string name;
    expr_ptr default_value; // null if none
    int line = 0, col = 0;
};

struct rune_def {
    std::string name;
    std::vector<param> params;
    std::vector<stmt_ptr> body;
    int line = 0, col = 0;
};

struct sigil_def {
    std::string name;
    std::vector<param> params;
    std::vector<stmt_ptr> body;
    int line = 0, col = 0;
};

struct program {
    std::vector<rune_def> runes;
    std::vector<sigil_def> sigils;
};

// ---- Debug dump (for tests / tooling) --------------------------------------

inline const char* binop_str(Tok op) {
    switch (op) {
        case Tok::Colon: return ":";  case Tok::Comma: return ",";
        case Tok::Split: return "<:"; case Tok::Merge: return ":>";
        case Tok::Plus: return "+";   case Tok::Minus: return "-";
        case Tok::Star: return "*";   case Tok::Slash: return "/";
        case Tok::Percent: return "%"; default: return "?";
    }
}

inline void dump_expr(const expr& e, std::string& out) {
    switch (e.k) {
        case expr::kind::number: out += std::to_string(e.number); break;
        case expr::kind::string: out += "\"" + e.text + "\""; break;
        case expr::kind::boolean: out += e.boolean ? "true" : "false"; break;
        case expr::kind::ident: out += e.text; break;
        case expr::kind::unary: out += "(-"; dump_expr(*e.a, out); out += ")"; break;
        case expr::kind::select: dump_expr(*e.a, out); out += "[" + std::to_string(e.index) + "]"; break;
        case expr::kind::binary:
            out += "("; dump_expr(*e.a, out); out += " "; out += binop_str(e.op); out += " ";
            dump_expr(*e.b, out); out += ")"; break;
        case expr::kind::call:
            out += e.text + "(";
            for (size_t k = 0; k < e.args.size(); ++k) {
                if (k) out += ", ";
                if (!e.args[k].name.empty()) out += e.args[k].name + "=";
                dump_expr(*e.args[k].value, out);
            }
            out += ")"; break;
    }
}

inline void dump_stmt(const stmt& s, std::string& out) {
    switch (s.k) {
        case stmt::kind::binding: out += s.name + " = "; dump_expr(*s.value, out); break;
        case stmt::kind::wire: dump_expr(*s.expr_, out); break;
        case stmt::kind::modulate:
            dump_expr(*s.source, out); out += " ~> ";
            for (size_t k = 0; k < s.target.parts.size(); ++k) { if (k) out += "."; out += s.target.parts[k]; }
            break;
        case stmt::kind::explicit_wire:
            dump_expr(*s.source, out);
            out += " -> " + s.target_node + "[" + std::to_string(s.target_input) + "]"; break;
        case stmt::kind::input:
            out += "input " + s.name + "(";
            for (size_t k = 0; k < s.args.size(); ++k) {
                if (k) out += ", ";
                if (!s.args[k].name.empty()) out += s.args[k].name + "=";
                dump_expr(*s.args[k].value, out);
            }
            out += ")"; break;
        case stmt::kind::out:
            out += "out "; dump_expr(*s.expr_, out);
            if (!s.terminal.empty()) out += " as " + s.terminal; break;
    }
}

inline std::string dump(const program& p) {
    std::string out;
    for (const auto& s : p.sigils) {
        out += "sigil " + s.name + "(";
        for (size_t k = 0; k < s.params.size(); ++k) { if (k) out += ", "; out += s.params[k].name; }
        out += ")\n";
        for (const auto& st : s.body) { out += "  "; dump_stmt(*st, out); out += "\n"; }
        out += "end\n";
    }
    for (const auto& r : p.runes) {
        out += "rune " + r.name + "(";
        for (size_t k = 0; k < r.params.size(); ++k) { if (k) out += ", "; out += r.params[k].name; }
        out += ")\n";
        for (const auto& st : r.body) { out += "  "; dump_stmt(*st, out); out += "\n"; }
        out += "end\n";
    }
    return out;
}

} // namespace xrune::galdr
