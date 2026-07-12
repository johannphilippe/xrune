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
#include "lexer.hpp"
#include "ast.hpp"
#include "diagnostic.hpp"
#include <string>
#include <vector>
#include <memory>

// Recursive-descent + precedence-climbing parser for Galdr (spec Appendix A).
// Produces an AST and a list of diagnostics; recovers at statement / definition
// boundaries so one error does not abort the whole file.

namespace xrune::galdr {

struct parse_result {
    program prog;
    diagnostics diags;
    bool ok() const { return diags.empty(); }
};

struct parser {
    std::vector<Token> toks;
    diagnostics& diags;
    size_t pos = 0;

    parser(std::vector<Token> t, diagnostics& d) : toks(std::move(t)), diags(d) {}

    // ---- token helpers ----
    const Token& cur() const { return toks[pos]; }
    const Token& peek(size_t k = 1) const {
        size_t j = pos + k; return toks[j < toks.size() ? j : toks.size() - 1];
    }
    bool at(Tok k) const { return cur().kind == k; }
    void adv() { if (pos + 1 < toks.size()) ++pos; }
    bool accept(Tok k) { if (at(k)) { adv(); return true; } return false; }

    void error(const std::string& m) { diags.push_back({m, cur().line, cur().col}); }
    void error_at(const std::string& m, int l, int c) { diags.push_back({m, l, c}); }

    Token expect(Tok k, const char* what) {
        if (at(k)) { Token t = cur(); adv(); return t; }
        error(std::string("expected ") + what + ", got " + tok_name(cur().kind));
        return cur();
    }

    void skip_newlines() { while (at(Tok::Newline)) adv(); }

    // Advance to the next likely statement/definition boundary.
    void synchronize() {
        while (!at(Tok::End) && !at(Tok::Newline) &&
               !at(Tok::KwEnd) && !at(Tok::KwRune) && !at(Tok::KwSigil))
            adv();
    }

    // ---- entry ----
    program parse_program() {
        program p;
        skip_newlines();
        while (!at(Tok::End)) {
            size_t before = pos;
            if (at(Tok::KwRune)) p.runes.push_back(parse_def<rune_def>());
            else if (at(Tok::KwSigil)) p.sigils.push_back(parse_def<sigil_def>());
            else { error("expected 'rune' or 'sigil'"); synchronize(); }
            if (pos == before) adv(); // guarantee progress
            skip_newlines();
        }
        return p;
    }

    // rune/sigil share structure.
    template <typename Def>
    Def parse_def() {
        Def d;
        d.line = cur().line; d.col = cur().col;
        adv(); // 'rune' / 'sigil'
        Token name = expect(Tok::Ident, "definition name");
        d.name = name.text;
        if (at(Tok::LParen)) d.params = parse_params();
        // body until 'end'
        skip_newlines();
        while (!at(Tok::KwEnd) && !at(Tok::End)) {
            size_t before = pos;
            stmt_ptr s = parse_statement();
            if (s) d.body.push_back(std::move(s));
            if (!at(Tok::Newline) && !at(Tok::KwEnd) && !at(Tok::End)) {
                error("expected end of line after statement");
                synchronize();
            }
            if (pos == before) adv();
            skip_newlines();
        }
        expect(Tok::KwEnd, "'end'");
        return d;
    }

    std::vector<param> parse_params() {
        std::vector<param> ps;
        expect(Tok::LParen, "'('");
        if (accept(Tok::RParen)) return ps;
        for (;;) {
            param pm;
            Token nm = expect(Tok::Ident, "parameter name");
            pm.name = nm.text; pm.line = nm.line; pm.col = nm.col;
            if (accept(Tok::Assign)) pm.default_value = parse_expr(true);
            ps.push_back(std::move(pm));
            if (accept(Tok::Comma)) continue;
            break;
        }
        expect(Tok::RParen, "')'");
        return ps;
    }

    // ---- statements ----
    stmt_ptr parse_statement() {
        if (at(Tok::KwInput)) return parse_input();
        if (at(Tok::KwOut)) return parse_out();

        // binding: IDENT '=' ...
        if (at(Tok::Ident) && peek().kind == Tok::Assign) {
            auto s = std::make_unique<stmt>(stmt::kind::binding);
            s->line = cur().line; s->col = cur().col;
            s->name = cur().text; adv(); adv(); // ident, '='
            s->value = parse_expr(false);
            return s;
        }

        // expression-led: bare wire, modulation, or explicit wire.
        int l = cur().line, c = cur().col;
        expr_ptr e = parse_expr(false);
        if (accept(Tok::Modulate)) {
            auto s = std::make_unique<stmt>(stmt::kind::modulate);
            s->line = l; s->col = c; s->source = std::move(e);
            s->target = parse_port_path();
            return s;
        }
        if (accept(Tok::Arrow)) {
            auto s = std::make_unique<stmt>(stmt::kind::explicit_wire);
            s->line = l; s->col = c; s->source = std::move(e);
            Token tn = expect(Tok::Ident, "target node name");
            s->target_node = tn.text;
            expect(Tok::LBracket, "'['");
            s->target_input = parse_index();
            expect(Tok::RBracket, "']'");
            return s;
        }
        auto s = std::make_unique<stmt>(stmt::kind::wire);
        s->line = l; s->col = c; s->expr_ = std::move(e);
        return s;
    }

    stmt_ptr parse_input() {
        auto s = std::make_unique<stmt>(stmt::kind::input);
        s->line = cur().line; s->col = cur().col;
        adv(); // 'input'
        Token nm = expect(Tok::Ident, "terminal name");
        s->name = nm.text;
        s->args = parse_call_args();
        return s;
    }

    stmt_ptr parse_out() {
        auto s = std::make_unique<stmt>(stmt::kind::out);
        s->line = cur().line; s->col = cur().col;
        adv(); // 'out'
        s->expr_ = parse_expr(false);
        if (accept(Tok::KwAs)) {
            Token t = expect(Tok::Ident, "terminal name after 'as'");
            s->terminal = t.text;
        }
        return s;
    }

    port_path parse_port_path() {
        port_path pp;
        pp.line = cur().line; pp.col = cur().col;
        Token first = expect(Tok::Ident, "port path (node.port)");
        pp.parts.push_back(first.text);
        while (accept(Tok::Dot)) {
            Token t = expect(Tok::Ident, "identifier after '.'");
            pp.parts.push_back(t.text);
        }
        if (pp.parts.size() < 2)
            error_at("modulation target must be node.port", pp.line, pp.col);
        return pp;
    }

    // ---- expressions (precedence climbing) ----
    static int binprec(Tok op) {
        switch (op) {
            case Tok::Merge:   return 1;
            case Tok::Split:   return 2;
            case Tok::Colon:   return 3;
            case Tok::Comma:   return 4;
            case Tok::Plus: case Tok::Minus: return 5;
            case Tok::Star: case Tok::Slash: case Tok::Percent: return 6;
            default: return 0;
        }
    }

    // in_args: comma separates call arguments here, so it is not the parallel
    // operator (spec §9 conflict C2). Grouping parens reset this to false.
    expr_ptr parse_expr(bool in_args) { return parse_binary(1, in_args); }

    expr_ptr parse_binary(int min_prec, bool in_args) {
        expr_ptr left = parse_unary(in_args);
        for (;;) {
            Tok op = cur().kind;
            int p = binprec(op);
            if (p == 0) break;
            if (op == Tok::Comma && in_args) break; // argument separator
            if (p < min_prec) break;
            int l = cur().line, c = cur().col;
            adv();
            expr_ptr right = parse_binary(p + 1, in_args); // left-associative
            auto n = std::make_unique<expr>(expr::kind::binary);
            n->line = l; n->col = c; n->op = op;
            n->a = std::move(left); n->b = std::move(right);
            left = std::move(n);
        }
        return left;
    }

    expr_ptr parse_unary(bool in_args) {
        if (at(Tok::Minus)) {
            int l = cur().line, c = cur().col; adv();
            auto n = std::make_unique<expr>(expr::kind::unary);
            n->line = l; n->col = c; n->op = Tok::Minus;
            n->a = parse_unary(in_args);
            return n;
        }
        return parse_postfix(in_args);
    }

    expr_ptr parse_postfix(bool in_args) {
        expr_ptr base = parse_primary(in_args);
        while (at(Tok::LBracket)) {
            int l = cur().line, c = cur().col; adv();
            int idx = parse_index();
            expect(Tok::RBracket, "']'");
            auto n = std::make_unique<expr>(expr::kind::select);
            n->line = l; n->col = c; n->index = idx; n->a = std::move(base);
            base = std::move(n);
        }
        return base;
    }

    expr_ptr parse_primary(bool /*in_args*/) {
        int l = cur().line, c = cur().col;
        if (accept(Tok::LParen)) {
            expr_ptr e = parse_binary(1, false); // grouping: comma is parallel
            expect(Tok::RParen, "')'");
            return e;
        }
        if (at(Tok::Number)) {
            auto n = std::make_unique<expr>(expr::kind::number);
            n->line = l; n->col = c; n->number = cur().number; adv();
            return n;
        }
        if (at(Tok::String)) {
            auto n = std::make_unique<expr>(expr::kind::string);
            n->line = l; n->col = c; n->text = cur().text; adv();
            return n;
        }
        if (at(Tok::KwTrue) || at(Tok::KwFalse)) {
            auto n = std::make_unique<expr>(expr::kind::boolean);
            n->line = l; n->col = c; n->boolean = at(Tok::KwTrue); adv();
            return n;
        }
        if (at(Tok::Ident)) {
            std::string name = cur().text; adv();
            if (at(Tok::LParen)) {
                auto n = std::make_unique<expr>(expr::kind::call);
                n->line = l; n->col = c; n->text = name;
                n->args = parse_call_args();
                return n;
            }
            auto n = std::make_unique<expr>(expr::kind::ident);
            n->line = l; n->col = c; n->text = name;
            return n;
        }
        error(std::string("expected an expression, got ") + tok_name(cur().kind));
        // Return a placeholder so higher levels keep parsing.
        auto n = std::make_unique<expr>(expr::kind::number);
        n->line = l; n->col = c; n->number = 0.0;
        return n;
    }

    std::vector<argument> parse_call_args() {
        std::vector<argument> args;
        expect(Tok::LParen, "'('");
        if (accept(Tok::RParen)) return args;
        for (;;) {
            argument a;
            a.line = cur().line; a.col = cur().col;
            if (at(Tok::Ident) && peek().kind == Tok::Assign) {
                a.name = cur().text; adv(); adv(); // ident '='
            }
            a.value = parse_expr(true); // comma is the arg separator
            args.push_back(std::move(a));
            if (accept(Tok::Comma)) continue;
            break;
        }
        expect(Tok::RParen, "')'");
        return args;
    }

    int parse_index() {
        if (at(Tok::Number)) {
            double v = cur().number; adv();
            int iv = static_cast<int>(v);
            if (static_cast<double>(iv) != v || iv < 0)
                error("channel index must be a non-negative integer");
            return iv;
        }
        error(std::string("expected a channel index, got ") + tok_name(cur().kind));
        return 0;
    }
};

inline parse_result parse(const std::string& src) {
    parse_result r;
    std::vector<Token> toks = lex(src, r.diags);
    parser p(std::move(toks), r.diags);
    r.prog = p.parse_program();
    return r;
}

} // namespace xrune::galdr
