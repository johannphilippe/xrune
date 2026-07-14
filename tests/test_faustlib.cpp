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

// faustlib: reach the Faust standard libraries from Xrune.
//
// The chain under test: index -> generated Faust source -> JIT compile -> a node
// whose ports and arity come from libfaust itself -> real audio.

#include "xrune/api.hpp"
#include "xrune/lang/compile.hpp"
#include "xrune/node/faust/faust_lib.hpp"
#include "xrune/audio/offline_backend.hpp"
#include "test_util.hpp"

#include <map>
#include <memory>
#include <string>

using namespace xrune;

int main() {
    // The index ships in data/faustlib.json; tests run from the build dir.
    faustlib_index idx;
    std::string err;
    bool have_index = idx.load("data/faustlib.json", err) ||
                      idx.load(std::string(XRUNE_SOURCE_DIR) + "/data/faustlib.json", err);

    XR_RUN("the library index loads");
    {
        XR_CHECK(have_index);
        if (!have_index) {
            std::fprintf(stderr, "  (%s)\n", err.c_str());
            XR_MAIN_REPORT();
        }
        XR_CHECK(idx.functions.size() > 100);
        XR_CHECK(idx.find("si.smoo") != nullptr);
        XR_CHECK(idx.find("ve.korg35LPF") != nullptr);
    }

    // ---- source generation -------------------------------------------------
    XR_RUN("generated Faust source is what we intend");
    {
        // Zero-argument function: no sliders at all.
        const std::string smoo = faustlib_source(*idx.find("si.smoo"), {});
        XR_CHECK(smoo.find("import(\"stdfaust.lib\");") != std::string::npos);
        XR_CHECK(smoo.find("process = si.smoo;") != std::string::npos);
        XR_CHECK(smoo.find("hslider") == std::string::npos);

        // Control parameters become hsliders, with the library author's ranges.
        const std::string korg = faustlib_source(*idx.find("ve.korg35LPF"), {});
        XR_CHECK(korg.find("process = ve.korg35LPF(") != std::string::npos);
        XR_CHECK(korg.find("hslider(\"normFreq\"") != std::string::npos);
        XR_CHECK(korg.find("hslider(\"Q\"") != std::string::npos);

        // An override replaces the default, not the range.
        const std::string q5 = faustlib_source(*idx.find("ve.korg35LPF"), {{"Q", 5.0}});
        XR_CHECK(q5.find("hslider(\"Q\", 5,") != std::string::npos);
    }

    // ---- compile-time arguments -------------------------------------------
    // fi.lowpass(N, fc): N is an order, NOT a slider. Faust would emit a
    // baffling error about source the user never wrote; we refuse first, with a
    // message that names the argument and shows the fix.
    XR_RUN("a compile-time argument must be supplied, and says so");
    {
        const faustlib_function* lp = idx.find("fi.lowpass");
        XR_CHECK(lp != nullptr);
        if (lp) {
            bool has_const = false;
            for (const auto& p : lp->params) has_const |= p.is_const;
            XR_CHECK(has_const);                 // Faust itself told the scanner so

            bool threw = false;
            std::string msg;
            try {
                (void)faustlib_source(*lp, {});  // N omitted
            } catch (const std::exception& e) {
                threw = true;
                msg = e.what();
            }
            XR_CHECK(threw);
            XR_CHECK(msg.find("compile-time") != std::string::npos);
            XR_CHECK(msg.find("N = ") != std::string::npos);   // shows the fix

            // With N supplied, it generates a literal -- not a slider.
            const std::string src = faustlib_source(*lp, {{"N", 3}});
            XR_CHECK(src.find("fi.lowpass(3,") != std::string::npos);
        }
    }

    // ---- the node: arity and ports come from libfaust, not from our parser --
    XR_RUN("faustlib node: ports discovered by libfaust");
    {
        std::unique_ptr<node> n = make_faustlib("ve.korg35LPF", {}, idx);
        XR_CHECK(n != nullptr);
        XR_CHECK(n->inputs_count() == 1);        // Faust says so, we did not guess
        XR_CHECK(n->outputs_count() == 1);
        XR_CHECK(n->params_count() == 2);

        const port_descriptor* pd = n->params();
        const std::string p0 = pd[0].name, p1 = pd[1].name;
        XR_CHECK(p0.find("normFreq") != std::string::npos ||
                 p1.find("normFreq") != std::string::npos);
        XR_CHECK(p0.find("Q") != std::string::npos || p1.find("Q") != std::string::npos);
    }

    // ---- the cache: compile once, share widely -----------------------------
    XR_RUN("the same function compiles exactly once");
    {
        const std::string src = faustlib_source(*idx.find("si.smoo"), {});
        auto a = faustlib_factory(src);
        auto b = faustlib_factory(src);
        XR_CHECK(a != nullptr);
        XR_CHECK(a == b);                        // the SAME factory, not a rebuild

        // Different compile-time arguments are a genuinely different program.
        auto c = faustlib_factory(faustlib_source(*idx.find("ve.korg35LPF"), {}));
        XR_CHECK(c != a);
    }

    // ---- it actually makes sound, through the language ---------------------
    XR_RUN("faustlib in a rune produces audio");
    {
        runtime rt;
        auto ob = std::make_unique<offline_backend>();
        offline_backend* p = ob.get();
        rt.use_backend(std::move(ob));
        rt.init({48000, 128, 0, 2, 16, 0});
        rt.start();

        // A sine through a Faust virtual-analog filter, straight from the library.
        lang::load_result r = lang::load(rt,
            "rune t()\n"
            "  out sine(220) : faustlib(\"ve.korg35LPF\") * 0.5\n"
            "end\n");
        if (!r.ok())
            for (const auto& d : r.diags) std::fprintf(stderr, "  DIAG %s\n", d.format().c_str());
        XR_CHECK(r.ok());
        XR_CHECK(!r.blueprints.empty());

        if (r.ok() && !r.blueprints.empty()) {
            voice v = rt.spawn(r.blueprints.front().second);
            XR_CHECK(v.valid());
            p->render(200);
            XR_CHECK(p->rms(0) > 0.0);           // the filter passed signal
            XR_CHECK(p->peak(0) < 2.0);          // ... and did not blow up
        }
    }

    XR_RUN("the filter's ports are addressable from the runtime");
    {
        runtime rt;
        auto ob = std::make_unique<offline_backend>();
        offline_backend* p = ob.get();
        rt.use_backend(std::move(ob));
        rt.init({48000, 128, 0, 2, 16, 0});
        rt.start();

        lang::load_result r = lang::load(rt,
            "rune t()\n"
            "  filter = faustlib(\"ve.korg35LPF\")\n"
            "  out sine(220) : filter * 0.5\n"
            "end\n");
        XR_CHECK(r.ok());

        if (r.ok() && !r.blueprints.empty()) {
            const blueprint_id id = r.blueprints.front().second;
            const blueprint_info* bi = rt.describe(id);

            // The Faust function's parameters are ordinary Xrune ports.
            bool found = false;
            for (const auto& ni : bi->nodes)
                if (ni.name == "filter") {
                    found = true;
                    XR_CHECK(ni.ports.size() == 2);
                }
            XR_CHECK(found);

            voice v = rt.spawn(id);
            p->render(50);
            const double before = p->rms(0);

            // Close the filter right down: the output must drop.
            const blueprint_info* info = rt.describe(id);
            std::string fq;
            for (const auto& ni : info->nodes)
                if (ni.name == "filter")
                    for (const auto& pt : ni.ports)
                        if (pt.name.find("normFreq") != std::string::npos) fq = pt.name;
            XR_CHECK(!fq.empty());
            XR_CHECK(rt.set(v, "filter", fq, 0.001));
            p->render(4);
            p->render(200);
            XR_CHECK(p->rms(0) < before);
        }
    }

    // ---- errors are diagnostics, not crashes -------------------------------
    XR_RUN("an unknown function is a clean compile error");
    {
        runtime rt;
        rt.use_backend(std::make_unique<offline_backend>());
        rt.init({48000, 128, 0, 2, 16, 0});
        rt.start();

        lang::load_result r = lang::load(rt,
            "rune t()\n  out sine(220) : faustlib(\"xx.nosuchfunction\")\nend\n");
        XR_CHECK(!r.ok());
        XR_CHECK(!r.diags.empty());
        XR_CHECK(r.diags.front().message.find("faustlib") != std::string::npos);
        XR_CHECK(r.diags.front().line > 0);      // it carries a source position
    }

    XR_MAIN_REPORT();
}
