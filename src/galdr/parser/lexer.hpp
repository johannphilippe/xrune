#pragma once
#include "token.hpp"
#include "diagnostic.hpp"
#include <string>
#include <vector>
#include <cstdlib>
#include <cctype>

// Hand-written lexer for Galdr. Longest-match; tracks line/col; emits Newline
// tokens only where they terminate a statement (suppressed inside ()/[] and
// after a continuation operator, spec §2).

namespace xrune::galdr {

struct lexer {
    const std::string& src;
    diagnostics& diags;
    size_t i = 0;
    int line = 1;
    int col = 1;
    int bracket_depth = 0;
    Tok last = Tok::Newline; // treat start-of-file like a fresh line

    lexer(const std::string& s, diagnostics& d) : src(s), diags(d) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        for (;;) {
            Token t = next();
            // Collapse consecutive newlines; drop a leading one.
            if (t.kind == Tok::Newline) {
                if (out.empty() || out.back().kind == Tok::Newline) continue;
            }
            const bool end = (t.kind == Tok::End);
            out.push_back(t);
            if (t.kind != Tok::Newline) last = t.kind;
            if (end) break;
        }
        return out;
    }

private:
    char cur() const { return i < src.size() ? src[i] : '\0'; }
    char peek(size_t k = 1) const { return (i + k) < src.size() ? src[i + k] : '\0'; }

    void advance() {
        if (i < src.size()) {
            if (src[i] == '\n') { ++line; col = 1; }
            else { ++col; }
            ++i;
        }
    }

    void error(const std::string& m, int l, int c) { diags.push_back({m, l, c}); }

    Token make(Tok k, int l, int c, std::string text = "") {
        Token t; t.kind = k; t.line = l; t.col = c; t.text = std::move(text); return t;
    }

    Token next() {
        for (;;) {
            // Handle newlines with continuation suppression.
            if (cur() == '\n') {
                const int l = line, c = col;
                advance();
                if (bracket_depth > 0 || continues_line(last)) continue; // continuation
                return make(Tok::Newline, l, c);
            }
            if (cur() == ' ' || cur() == '\t' || cur() == '\r') { advance(); continue; }

            // Comments.
            if (cur() == '/' && peek() == '/') { while (cur() && cur() != '\n') advance(); continue; }
            if (cur() == '/' && peek() == '*') {
                const int l = line, c = col;
                advance(); advance();
                while (cur() && !(cur() == '*' && peek() == '/')) advance();
                if (!cur()) { error("unterminated block comment", l, c); return make(Tok::End, line, col); }
                advance(); advance();
                continue;
            }
            break;
        }

        const int l = line, c = col;
        if (i >= src.size()) return make(Tok::End, l, c);

        const char ch = cur();

        if (std::isdigit((unsigned char)ch) ||
            (ch == '.' && std::isdigit((unsigned char)peek())))
            return number(l, c);

        if (std::isalpha((unsigned char)ch) || ch == '_') return ident(l, c);

        if (ch == '"') return string(l, c);

        // Multi-char operators (longest match), then single-char.
        switch (ch) {
            case '<':
                if (peek() == ':') { advance(); advance(); return make(Tok::Split, l, c); }
                advance(); error("unexpected '<' (did you mean '<:' ?)", l, c); return make(Tok::Unknown, l, c);
            case ':':
                if (peek() == '>') { advance(); advance(); return make(Tok::Merge, l, c); }
                advance(); return make(Tok::Colon, l, c);
            case '~':
                if (peek() == '>') { advance(); advance(); return make(Tok::Modulate, l, c); }
                advance(); return make(Tok::Tilde, l, c);
            case '-':
                if (peek() == '>') { advance(); advance(); return make(Tok::Arrow, l, c); }
                advance(); return make(Tok::Minus, l, c);
            case '(': advance(); ++bracket_depth; return make(Tok::LParen, l, c);
            case ')': advance(); if (bracket_depth) --bracket_depth; return make(Tok::RParen, l, c);
            case '[': advance(); ++bracket_depth; return make(Tok::LBracket, l, c);
            case ']': advance(); if (bracket_depth) --bracket_depth; return make(Tok::RBracket, l, c);
            case '=': advance(); return make(Tok::Assign, l, c);
            case ',': advance(); return make(Tok::Comma, l, c);
            case '.': advance(); return make(Tok::Dot, l, c);
            case '+': advance(); return make(Tok::Plus, l, c);
            case '*': advance(); return make(Tok::Star, l, c);
            case '/': advance(); return make(Tok::Slash, l, c);
            case '%': advance(); return make(Tok::Percent, l, c);
        }

        advance();
        error(std::string("unexpected character '") + ch + "'", l, c);
        return make(Tok::Unknown, l, c);
    }

    Token number(int l, int c) {
        size_t start = i;
        if (cur() == '0' && (peek() == 'x' || peek() == 'X')) {
            advance(); advance();
            while (std::isxdigit((unsigned char)cur())) advance();
        } else {
            while (std::isdigit((unsigned char)cur())) advance();
            if (cur() == '.') { advance(); while (std::isdigit((unsigned char)cur())) advance(); }
            if (cur() == 'e' || cur() == 'E') {
                advance();
                if (cur() == '+' || cur() == '-') advance();
                while (std::isdigit((unsigned char)cur())) advance();
            }
        }
        std::string text = src.substr(start, i - start);
        Token t = make(Tok::Number, l, c, text);
        t.number = std::strtod(text.c_str(), nullptr);
        return t;
    }

    Token ident(int l, int c) {
        size_t start = i;
        while (std::isalnum((unsigned char)cur()) || cur() == '_') advance();
        std::string text = src.substr(start, i - start);
        Tok k = Tok::Ident;
        if (text == "rune") k = Tok::KwRune;
        else if (text == "sigil") k = Tok::KwSigil;
        else if (text == "end") k = Tok::KwEnd;
        else if (text == "input") k = Tok::KwInput;
        else if (text == "out") k = Tok::KwOut;
        else if (text == "as") k = Tok::KwAs;
        else if (text == "true") k = Tok::KwTrue;
        else if (text == "false") k = Tok::KwFalse;
        return make(k, l, c, text);
    }

    Token string(int l, int c) {
        advance(); // opening quote
        std::string s;
        while (cur() && cur() != '"') {
            if (cur() == '\\' && peek()) { advance(); s += cur(); advance(); continue; }
            if (cur() == '\n') { error("unterminated string", l, c); break; }
            s += cur(); advance();
        }
        if (cur() == '"') advance();
        else error("unterminated string", l, c);
        return make(Tok::String, l, c, s);
    }
};

inline std::vector<Token> lex(const std::string& src, diagnostics& diags) {
    return lexer(src, diags).run();
}

} // namespace xrune::galdr
