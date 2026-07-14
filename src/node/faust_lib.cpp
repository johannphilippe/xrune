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

#include "xrune/node/faust/faust_lib.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace xrune {

// ---------------------------------------------------------------------------
// The index
// ---------------------------------------------------------------------------

bool faustlib_index::load_json(const std::string& text, std::string& err) {
    json::value root;
    if (!json::parse(text, root, err)) return false;
    if (!root.is_object()) { err = "faustlib index: top level must be an object"; return false; }

    functions.clear();
    for (const auto& kv : root.obj) {
        const json::value& v = kv.second;
        if (!v.is_object()) continue;

        faustlib_function fn;
        fn.name = kv.first;
        fn.lib = v.text("lib");

        if (const json::value* ps = v.find("params"); ps && ps->is_array()) {
            for (const json::value& p : ps->arr) {
                if (!p.is_object()) continue;
                faustlib_param fp;
                fp.name = p.text("name");
                if (fp.name.empty()) continue;
                fp.doc = p.text("doc");

                if (const json::value* c = p.find("const"))
                    fp.is_const = (c->k == json::value::kind::boolean) && c->boolean;
                if (const json::value* t = p.find("from_test"))
                    fp.from_test = (t->k == json::value::kind::boolean) && t->boolean;

                fp.default_value = p.num("default", 0.0);
                fp.min_value = p.num("min", -1e6);
                fp.max_value = p.num("max", 1e6);
                fp.step = p.num("step", 0.001);
                fn.params.push_back(std::move(fp));
            }
        }
        functions[fn.name] = std::move(fn);
    }
    return true;
}

bool faustlib_index::load(const std::string& path, std::string& err) {
    std::ifstream f(path);
    if (!f) { err = "faustlib index: cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return load_json(ss.str(), err);
}

namespace {

// Directory holding the running executable, or "" if we cannot tell.
//
// This is what makes the index RELOCATABLE. The configure-time
// XRUNE_FAUSTLIB_JSON_PATH is baked from CMAKE_INSTALL_PREFIX, so a later
// `cmake --install --prefix /somewhere/else` leaves it pointing at /usr/local --
// the same trap that bit the rpath. Resolving relative to the executable is the
// $ORIGIN equivalent, and works from any prefix.
std::string exe_dir() {
    std::string path;
#if defined(__linux__)
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) { buf[n] = '\0'; path = buf; }
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) path = buf;
#endif
    if (path.empty()) return {};
    const size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? std::string{} : path.substr(0, slash);
}

} // namespace

faustlib_index& faustlib_index::standard() {
    static faustlib_index idx;
    static bool tried = false;
    if (tried) return idx;
    tried = true;

    std::vector<std::string> candidates;

    // 1. An explicit override always wins.
    if (const char* env = std::getenv("XRUNE_FAUSTLIB_JSON"))
        candidates.emplace_back(env);

    // 2. Relative to the executable -- relocatable, so it works from ANY install
    //    prefix, and from the build tree.
    if (const std::string dir = exe_dir(); !dir.empty()) {
        candidates.emplace_back(dir + "/../share/xrune/faustlib.json");  // installed
        candidates.emplace_back(dir + "/data/faustlib.json");            // build tree
    }

    // 3. The configure-time install path (correct only when the prefix used at
    //    install time matches the one used at configure time).
#ifdef XRUNE_FAUSTLIB_JSON_PATH
    candidates.emplace_back(XRUNE_FAUSTLIB_JSON_PATH);
#endif

    // 4. Relative to the working directory (running from the source tree).
    candidates.emplace_back("data/faustlib.json");

    std::string err;
    for (const auto& c : candidates)
        if (idx.load(c, err)) return idx;

    // Left empty: make_faustlib() reports the failure with the search advice,
    // rather than guessing and producing a node with the wrong arity.
    return idx;
}

// ---------------------------------------------------------------------------
// Source generation
// ---------------------------------------------------------------------------

namespace {

// Faust wants a plain literal; %g would render 1e+06 as "1e+06", which Faust
// accepts, but integers must not gain a spurious ".0" for a const arg.
std::string num(double v) {
    char buf[40];
    if (v == static_cast<long long>(v) && std::fabs(v) < 1e15)
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    else
        std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

} // namespace

std::string faustlib_source(const faustlib_function& fn,
                            const std::map<std::string, double>& overrides) {
    std::vector<std::string> args;
    args.reserve(fn.params.size());

    for (const faustlib_param& p : fn.params) {
        auto it = overrides.find(p.name);

        if (p.is_const) {
            // A compile-time argument CANNOT be a slider. Refuse clearly here
            // rather than let libfaust emit "not a constant expression" about a
            // source file the user never wrote.
            if (it == overrides.end())
                throw std::runtime_error(
                    "faustlib(\"" + fn.name + "\"): '" + p.name +
                    "' is a compile-time argument (" +
                    (p.doc.empty() ? std::string("a constant") : p.doc) +
                    ") and must be given a literal, e.g. faustlib(\"" + fn.name +
                    "\", " + p.name + " = 3)");
            args.push_back(num(it->second));
            continue;
        }

        // A control parameter becomes an hslider, which libfaust then reports
        // back to us through APIUI as a real Xrune port (name, range, default).
        const double def = (it != overrides.end()) ? it->second : p.default_value;
        args.push_back("hslider(\"" + p.name + "\", " + num(def) + ", " +
                       num(p.min_value) + ", " + num(p.max_value) + ", " +
                       num(p.step) + ")");
    }

    std::string call = fn.name;
    if (!args.empty()) {
        call += "(";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) call += ", ";
            call += args[i];
        }
        call += ")";
    }

    return "import(\"stdfaust.lib\");\nprocess = " + call + ";\n";
}

// ---------------------------------------------------------------------------
// The factory cache
// ---------------------------------------------------------------------------

std::shared_ptr<faust_factory> faustlib_factory(const std::string& source) {
    // Keyed by the generated source, so the same function with the same
    // arguments compiles exactly once no matter how many nodes use it.
    static std::map<std::string, std::weak_ptr<faust_factory>> cache;
    static std::mutex mtx;

    std::lock_guard<std::mutex> lock(mtx);

    auto it = cache.find(source);
    if (it != cache.end()) {
        if (auto alive = it->second.lock()) return alive;   // still in use: share it
        cache.erase(it);                                    // last user went away
    }

    auto fac = std::make_shared<faust_factory>(source, /*is_file=*/false, "-double");
    cache[source] = fac;
    return fac;
}

// ---------------------------------------------------------------------------
// The node
// ---------------------------------------------------------------------------

std::unique_ptr<node> make_faustlib(const std::string& function_name,
                                    const std::map<std::string, double>& overrides,
                                    const faustlib_index& index) {
    // The index is REQUIRED. Without it we do not know the function's arguments,
    // and emitting it unapplied (`process = ve.korg35LPF;`) does not fail -- Faust
    // silently treats the missing arguments as extra AUDIO INPUTS, producing a
    // node with the wrong arity. A loud error beats a graph that is quietly wrong.
    if (index.functions.empty())
        throw std::runtime_error(
            "faustlib: the Faust library index was not found, so '" + function_name +
            "' cannot be resolved. Set $XRUNE_FAUSTLIB_JSON, install Xrune (the "
            "index goes to <prefix>/share/xrune/faustlib.json), or generate it:\n"
            "  python3 tools/faustlib_scan.py --verify -o data/faustlib.json");

    const faustlib_function* fn = index.find(function_name);
    if (!fn)
        throw std::runtime_error(
            "faustlib: unknown function '" + function_name + "'. It is not among the " +
            std::to_string(index.functions.size()) +
            " verified functions in the index. Check the spelling (it is "
            "'<lib>.<fn>', e.g. \"ve.korg35LPF\"), or regenerate the index with "
            "tools/faustlib_scan.py.");

    const std::string src = faustlib_source(*fn, overrides);
    return std::make_unique<faust_jit>(faustlib_factory(src));
}

} // namespace xrune
