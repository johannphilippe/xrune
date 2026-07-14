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

// Voice-end events: the push half of the lifetime API.
//
// The two properties that matter:
//   1. the callback runs on the CONTROL thread (inside pump()), never on the
//      audio thread -- so a host may allocate/lock/log in it without glitching;
//   2. the reason is accurate, and the handle matches what the host was given.

#include "xrune/api.hpp"
#include "xrune/node/standard_nodes.hpp"
#include "xrune/audio/offline_backend.hpp"
#include "test_util.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace xrune;

static constexpr size_t SR = 48000, BS = 128;

struct rig {
    runtime rt;
    offline_backend* ob = nullptr;
    blueprint_id id = invalid_blueprint;

    rig() {
        auto o = std::make_unique<offline_backend>();
        ob = o.get();
        rt.use_backend(std::move(o));
        rt.init({SR, BS, 0, 2, 32, 0});
        rt.start();

        blueprint_builder b("v");
        b.add<oscillator>("osc", 440.0)
         .add<gain>("amp", 0.25)
         .connect("osc", 0, "amp", 0)
         .output("amp");
        id = rt.register_blueprint(b);
    }
};

int main() {
    // ---- the callback fires on the control thread, not the audio thread ----
    XR_RUN("callback fires in pump(), never during render");
    {
        rig g;
        std::vector<voice_event> got;
        bool rendering = false;
        bool fired_during_render = false;

        g.rt.on_voice_end([&](const voice_event& e) {
            if (rendering) fired_during_render = true;   // would be an audio-thread call
            got.push_back(e);
        });

        voice v = g.rt.spawn(g.id);
        XR_CHECK(v.valid());
        g.rt.kill(v);

        // Render past the reap. The audio thread only pushes a record onto the
        // lock-free queue; it must NOT invoke user code.
        rendering = true;
        g.ob->render(4);
        rendering = false;

        XR_CHECK(!fired_during_render);
        XR_CHECK(got.empty());          // nothing delivered yet -- pump() has not run

        const size_t n = g.rt.pump();   // ... and now it is
        XR_CHECK(n == 1);
        XR_CHECK(got.size() == 1);
        XR_CHECK(!fired_during_render);
    }

    // ---- the event identifies the exact voice the host was handed ----------
    XR_RUN("event carries the caller's handle and blueprint");
    {
        rig g;
        std::vector<voice_event> got;
        g.rt.on_voice_end([&](const voice_event& e) { got.push_back(e); });

        voice v = g.rt.spawn(g.id);
        g.rt.kill(v);
        g.ob->render(4);
        g.rt.pump();

        XR_CHECK(got.size() == 1);
        XR_CHECK(got[0].v.handle.slot == v.handle.slot);
        XR_CHECK(got[0].v.handle.generation == v.handle.generation);  // pre-recycle
        XR_CHECK(got[0].v.blueprint == g.id);
        XR_CHECK(got[0].v.valid());
    }

    // ---- the reason is accurate --------------------------------------------
    XR_RUN("reason: killed");
    {
        rig g;
        std::vector<voice_event> got;
        g.rt.on_voice_end([&](const voice_event& e) { got.push_back(e); });

        voice v = g.rt.spawn(g.id);
        g.rt.kill(v);
        g.ob->render(4);
        g.rt.pump();

        XR_CHECK(got.size() == 1);
        XR_CHECK(got[0].reason == voice_end_reason::killed);
        XR_CHECK(std::string(to_string(got[0].reason)) == "killed");
    }

    XR_RUN("reason: timed_out");
    {
        rig g;
        std::vector<voice_event> got;
        g.rt.on_voice_end([&](const voice_event& e) { got.push_back(e); });

        spawn_options o;
        o.life = {lifetime_kind::timed, 3, 1e-5, 0};
        voice v = g.rt.spawn(g.id, o);
        XR_CHECK(v.valid());

        g.ob->render(2);
        g.rt.pump();
        XR_CHECK(got.empty());          // not yet -- still within its ttl

        g.ob->render(4);
        g.rt.pump();
        XR_CHECK(got.size() == 1);
        XR_CHECK(got[0].reason == voice_end_reason::timed_out);
        XR_CHECK(!g.rt.alive(v));
    }

    XR_RUN("reason: silent");
    {
        rig g;
        std::vector<voice_event> got;
        g.rt.on_voice_end([&](const voice_event& e) { got.push_back(e); });

        // A silent voice (gain 0) reaped after 2 consecutive quiet blocks.
        blueprint_builder b("quiet");
        b.add<oscillator>("osc", 440.0)
         .add<gain>("amp", 0.0)
         .connect("osc", 0, "amp", 0)
         .output("amp");
        blueprint_id qid = g.rt.register_blueprint(b);

        spawn_options o;
        o.life = {lifetime_kind::until_silent, 0, 1e-5, 2};
        voice v = g.rt.spawn(qid, o);
        XR_CHECK(v.valid());

        g.ob->render(6);
        g.rt.pump();
        XR_CHECK(got.size() == 1);
        XR_CHECK(got[0].reason == voice_end_reason::silent);
        XR_CHECK(got[0].v.blueprint == qid);
    }

    // ---- many voices ending in one block -----------------------------------
    XR_RUN("every ended voice is reported exactly once");
    {
        rig g;
        std::vector<voice_event> got;
        g.rt.on_voice_end([&](const voice_event& e) { got.push_back(e); });

        std::vector<voice> vs;
        for (int i = 0; i < 8; ++i) {
            spawn_options o;
            o.life = {lifetime_kind::timed, 2, 1e-5, 0};
            vs.push_back(g.rt.spawn(g.id, o));
        }
        g.ob->render(6);
        g.rt.pump();

        XR_CHECK(got.size() == 8);
        for (const auto& e : got) XR_CHECK(e.reason == voice_end_reason::timed_out);
        for (const auto& v : vs)  XR_CHECK(!g.rt.alive(v));

        g.rt.pump();                    // draining again reports nothing new
        XR_CHECK(got.size() == 8);
    }

    // ---- the callback may re-enter the runtime -----------------------------
    // The slot is recycled BEFORE the callback runs, so a host can respawn from
    // inside the notification (a sequencer chaining the next note).
    XR_RUN("callback may spawn a replacement voice");
    {
        rig g;
        int ended = 0;
        std::vector<voice> replacements;

        g.rt.on_voice_end([&](const voice_event&) {
            if (++ended > 3) return;                    // chain a few, then stop
            replacements.push_back(g.rt.spawn(g.id, [] {
                spawn_options o;
                o.life = {lifetime_kind::timed, 2, 1e-5, 0};
                return o;
            }()));
        });

        spawn_options o;
        o.life = {lifetime_kind::timed, 2, 1e-5, 0};
        g.rt.spawn(g.id, o);

        for (int i = 0; i < 6; ++i) { g.ob->render(4); g.rt.pump(); }

        XR_CHECK(ended >= 3);
        for (const auto& v : replacements) XR_CHECK(v.valid());
    }

    // ---- no callback installed: pump() still works -------------------------
    XR_RUN("pump() without a callback still reclaims");
    {
        rig g;
        voice v = g.rt.spawn(g.id);
        g.rt.kill(v);
        g.ob->render(4);
        XR_CHECK(g.rt.pump() == 1);
        XR_CHECK(!g.rt.alive(v));
        XR_CHECK(g.rt.active_voices() == 0);
    }

    XR_MAIN_REPORT();
}
