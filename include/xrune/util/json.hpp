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
// Deliberately not a third-party dependency: Xrune stays self-contained (the
// same call as the radix-2 FFT and the Xrune parser). This is *control-thread*
// code — it allocates freely and must never run on the audio thread.
//
// Scope: exactly what blueprint serialization needs — objects, arrays, strings,
// numbers, booleans, null. Objects preserve insertion order, so a round-trip is
// diffable. Strict JSON: no comments, no trailing commas.
//
// Implementation: src/util/json.cpp

#include <string>
#include <vector>
#include <utility>

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

    static value make_array();
    static value make_object();

    bool is_object() const { return k == kind::object; }
    bool is_array()  const { return k == kind::array;  }
    bool is_number() const { return k == kind::number; }
    bool is_string() const { return k == kind::string; }

    // Object access. Returns nullptr when absent (or when this isn't an object).
    const value* find(const std::string& key) const;
    void set(const std::string& key, value v);
    void push(value v);

    // Typed reads with a fallback, so callers don't repeat null checks.
    double      num(const std::string& key, double def = 0.0) const;
    long        integer(const std::string& key, long def = 0) const;
    std::string text(const std::string& key, const std::string& def = "") const;
};

// Serialize. indent = 0 produces a single compact line.
std::string dump(const value& v, int indent = 2);

// Parse `text` into `out`. Returns false and fills `err` (with a line number)
// on failure.
bool parse(const std::string& text, value& out, std::string& err);

} // namespace xrune::json
