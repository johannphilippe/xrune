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

// A small, self-contained JSON value + recursive-descent parser + writer.
//
// Deliberately not a third-party dependency: Xrune stays self-contained (same
// call as the radix-2 FFT and the Galdr lexer/parser). This is *control-thread*
// code — it allocates freely and must never be touched from the audio thread.
//
// Scope: exactly what blueprint serialization needs — objects, arrays, strings,
// numbers, booleans, null. Objects preserve insertion order so a round-trip is
// diffable. No comments, no trailing commas (strict JSON).

#include <string>
#include <vector>
#include <utility>
#include <cstdio>
#include <cmath>
#include <cstdlib>

namespace xrune::json {

struct value;
using object_t = std::vector<std::pair<std::string, value>>; // ordered
using array_t  = std::vector<value>;

struct value {
    enum class kind { null, boolean, number, string, array, object };

    kind k = kind::null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    array_t arr;
    object_t obj;

    value() = default;
    value(double n) : k(kind::number), number(n) {}
    value(bool b) : k(kind::boolean), boolean(b) {}
    value(const char* s) : k(kind::string), str(s) {}
    value(std::string s) : k(kind::string), str(std::move(s)) {}

    static value make_array()  { value v; v.k = kind::array;  return v; }
    static value make_object() { value v; v.k = kind::object; return v; }

    bool is_object() const { return k == kind::object; }
    bool is_array()  const { return k == kind::array;  }
    bool is_number() const { return k == kind::number; }
    bool is_string() const { return k == kind::string; }

    // Object access. Returns nullptr when absent (or when this isn't an object).
    const value* find(const std::string& key) const {
        if (k != kind::object) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    void set(const std::string& key, value v) { k = kind::object; obj.emplace_back(key, std::move(v)); }
    void push(value v) { k = kind::array; arr.push_back(std::move(v)); }

    // Typed reads with a fallback, so callers don't repeat null checks.
    double num(const std::string& key, double def = 0.0) const {
        const value* v = find(key);
        return (v && v->is_number()) ? v->number : def;
    }
    long integer(const std::string& key, long def = 0) const {
        const value* v = find(key);
        return (v && v->is_number()) ? static_cast<long>(std::llround(v->number)) : def;
    }
    std::string text(const std::string& key, const std::string& def = "") const {
        const value* v = find(key);
        return (v && v->is_string()) ? v->str : def;
    }
};

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

inline void escape_to(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else out += c;
        }
    }
    out += '"';
}

// Numbers: emit integers without a decimal point (so "size": 1024, not 1024.0).
// Otherwise use the *shortest* representation that still parses back to exactly
// the same double — %.17g always round-trips but prints 0.3 as
// 0.29999999999999999, which is exact and unreadable. Try increasing precision
// until the value survives a parse, so a human-authored 0.3 stays "0.3".
inline void number_to(std::string& out, double n) {
    char buf[40];
    if (std::isfinite(n) && n == std::floor(n) && std::fabs(n) < 1e15) {
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n));
        out += buf;
        return;
    }
    for (int prec = 15; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, n);
        if (std::strtod(buf, nullptr) == n) break;   // exact round-trip
    }
    out += buf;
}

inline void dump_to(std::string& out, const value& v, int indent, int depth) {
    const bool pretty = indent > 0;
    const std::string pad  = pretty ? std::string(size_t(indent) * size_t(depth + 1), ' ') : "";
    const std::string pad0 = pretty ? std::string(size_t(indent) * size_t(depth), ' ') : "";
    const char* nl = pretty ? "\n" : "";

    switch (v.k) {
        case value::kind::null:    out += "null"; break;
        case value::kind::boolean: out += v.boolean ? "true" : "false"; break;
        case value::kind::number:  number_to(out, v.number); break;
        case value::kind::string:  escape_to(out, v.str); break;
        case value::kind::array:
            if (v.arr.empty()) { out += "[]"; break; }
            out += "["; out += nl;
            for (size_t i = 0; i < v.arr.size(); ++i) {
                out += pad;
                dump_to(out, v.arr[i], indent, depth + 1);
                if (i + 1 < v.arr.size()) out += ",";
                out += nl;
            }
            out += pad0; out += "]";
            break;
        case value::kind::object:
            if (v.obj.empty()) { out += "{}"; break; }
            out += "{"; out += nl;
            for (size_t i = 0; i < v.obj.size(); ++i) {
                out += pad;
                escape_to(out, v.obj[i].first);
                out += pretty ? ": " : ":";
                dump_to(out, v.obj[i].second, indent, depth + 1);
                if (i + 1 < v.obj.size()) out += ",";
                out += nl;
            }
            out += pad0; out += "}";
            break;
    }
}

inline std::string dump(const value& v, int indent = 2) {
    std::string out;
    dump_to(out, v, indent, 0);
    return out;
}

// ---------------------------------------------------------------------------
// Parsing (recursive descent, strict)
// ---------------------------------------------------------------------------

struct parser {
    const std::string& src;
    size_t pos = 0;
    std::string error;

    explicit parser(const std::string& s) : src(s) {}

    void skip_ws() {
        while (pos < src.size() &&
               (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r'))
            ++pos;
    }
    bool eof() const { return pos >= src.size(); }
    char peek() const { return pos < src.size() ? src[pos] : '\0'; }

    bool fail(const std::string& msg) {
        if (error.empty()) {
            // Report a line number: cheaper to count than to track.
            size_t line = 1;
            for (size_t i = 0; i < pos && i < src.size(); ++i)
                if (src[i] == '\n') ++line;
            error = "line " + std::to_string(line) + ": " + msg;
        }
        return false;
    }

    bool literal(const char* lit) {
        size_t n = std::char_traits<char>::length(lit);
        if (src.compare(pos, n, lit) != 0) return fail(std::string("expected '") + lit + "'");
        pos += n;
        return true;
    }

    bool parse_string(std::string& out) {
        if (peek() != '"') return fail("expected string");
        ++pos;
        out.clear();
        while (true) {
            if (eof()) return fail("unterminated string");
            char c = src[pos++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (eof()) return fail("unterminated escape");
            char e = src[pos++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    if (pos + 4 > src.size()) return fail("bad \\u escape");
                    unsigned code = std::strtoul(src.substr(pos, 4).c_str(), nullptr, 16);
                    pos += 4;
                    // Minimal UTF-8 encoding; surrogate pairs are not needed here.
                    if (code < 0x80) out += static_cast<char>(code);
                    else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: return fail("invalid escape");
            }
        }
    }

    bool parse_value(value& out) {
        skip_ws();
        if (eof()) return fail("unexpected end of input");
        char c = peek();

        if (c == '{') {
            ++pos;
            out = value::make_object();
            skip_ws();
            if (peek() == '}') { ++pos; return true; }
            while (true) {
                skip_ws();
                std::string key;
                if (!parse_string(key)) return false;
                skip_ws();
                if (peek() != ':') return fail("expected ':'");
                ++pos;
                value v;
                if (!parse_value(v)) return false;
                out.obj.emplace_back(std::move(key), std::move(v));
                skip_ws();
                if (peek() == ',') { ++pos; continue; }
                if (peek() == '}') { ++pos; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++pos;
            out = value::make_array();
            skip_ws();
            if (peek() == ']') { ++pos; return true; }
            while (true) {
                value v;
                if (!parse_value(v)) return false;
                out.arr.push_back(std::move(v));
                skip_ws();
                if (peek() == ',') { ++pos; continue; }
                if (peek() == ']') { ++pos; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string s;
            if (!parse_string(s)) return false;
            out = value(std::move(s));
            return true;
        }
        if (c == 't') { if (!literal("true"))  return false; out = value(true);  return true; }
        if (c == 'f') { if (!literal("false")) return false; out = value(false); return true; }
        if (c == 'n') { if (!literal("null"))  return false; out = value();      return true; }

        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* start = src.c_str() + pos;
            char* end = nullptr;
            double d = std::strtod(start, &end);
            if (end == start) return fail("bad number");
            pos += static_cast<size_t>(end - start);
            out = value(d);
            return true;
        }
        return fail("unexpected character");
    }
};

// Parse `text` into `out`. Returns false and fills `err` on failure.
inline bool parse(const std::string& text, value& out, std::string& err) {
    parser p(text);
    if (!p.parse_value(out)) { err = p.error; return false; }
    p.skip_ws();
    if (!p.eof()) { err = "trailing content after top-level value"; return false; }
    return true;
}

} // namespace xrune::json
