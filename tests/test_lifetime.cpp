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

#include "core.hpp"
#include "standard_nodes.hpp"
#include "blueprint.hpp"
#include "schedule.hpp"
#include "instance.hpp"
#include "instance_manager.hpp"
#include "engine.hpp"
#include "offline_backend.hpp"
#include "test_util.hpp"
#include <memory>

using namespace xrune;

static graph_blueprint make_voice(double freq, double g) {
    graph_blueprint bp;
    size_t osc = bp.add_named<oscillator>("osc", freq);
    size_t gn = bp.add_named<gain>("amp", g);
    bp.connect(osc, 0, gn, 0);
    bp.set_output(gn);
    return bp;
}

// Build an engine wired to an offline backend; returns the backend pointer.
static offline_backend* attach_offline(engine& eng, size_t sr, size_t bs, size_t maxi) {
    auto ob = std::make_unique<offline_backend>();
    offline_backend* p = ob.get();
    eng.use_backend(std::move(ob));
    eng.init(sr, bs, 0, 2, maxi);
    return p;
}

int main() {
    const size_t sr = 48000, bs = 128;

    // --- name resolution ---
    XR_RUN("blueprint name resolution");
    {
        graph_blueprint bp = make_voice(440.0, 0.5);
        XR_CHECK(bp.find_node("osc") == 0);
        XR_CHECK(bp.find_node("amp") == 1);
        XR_CHECK(bp.find_node("nope") == -1);
    }

    // --- spawn / kill / reclaim + voice count + slot reuse ---
    XR_RUN("spawn/kill/reclaim");
    {
        graph_blueprint bp = make_voice(440.0, 0.25);
        compiled_schedule sched = compile(bp, bs);

        engine eng;
        offline_backend* ob = attach_offline(eng, sr, bs, 8);

        instance_handle a = eng.spawn(sched);
        instance_handle b = eng.spawn(sched);
        instance_handle c = eng.spawn(sched);
        XR_CHECK(a.valid() && b.valid() && c.valid());
        XR_CHECK(eng.mgr.in_use() == 3);

        ob->render(1);                 // processes the 3 activates
        XR_CHECK(eng.active_count() == 3);

        eng.kill(b);
        ob->render(1);                 // processes the kill -> release
        XR_CHECK(eng.active_count() == 2);

        size_t reclaimed = eng.reclaim();
        XR_CHECK(reclaimed == 1);
        XR_CHECK(eng.mgr.in_use() == 2);

        // Killed handle is now stale/invalid; a and c remain valid.
        XR_CHECK(!eng.is_valid(b));
        XR_CHECK(eng.is_valid(a));
        XR_CHECK(eng.is_valid(c));

        // Next spawn reuses the freed slot but with a fresh generation.
        instance_handle d = eng.spawn(sched);
        XR_CHECK(d.valid());
        XR_CHECK(d.slot == b.slot);       // recycled slot
        XR_CHECK(d.generation != b.generation);
        ob->render(1);
        XR_CHECK(eng.active_count() == 3);
    }

    // --- stale handle is dropped, does not disturb the reused slot ---
    XR_RUN("stale handle dropped");
    {
        graph_blueprint bp = make_voice(440.0, 0.25);
        compiled_schedule sched = compile(bp, bs);

        engine eng;
        offline_backend* ob = attach_offline(eng, sr, bs, 4);

        instance_handle a = eng.spawn(sched);
        ob->render(1);
        eng.kill(a);
        ob->render(1);
        eng.reclaim();

        instance_handle b = eng.spawn(sched); // reuses a's slot, new generation
        ob->render(1);
        XR_CHECK(b.slot == a.slot);
        XR_CHECK(eng.active_count() == 1);

        // Command on the stale handle must be ignored (no crash, b untouched).
        eng.set_parameter(a, 0, 0, 999.0);
        eng.kill(a);
        ob->render(2);
        XR_CHECK(eng.active_count() == 1);    // b still alive
        XR_CHECK(eng.is_valid(b));
    }

    // --- timed auto-reap ---
    XR_RUN("timed auto-reap");
    {
        graph_blueprint bp = make_voice(440.0, 0.25);
        compiled_schedule sched = compile(bp, bs);

        engine eng;
        offline_backend* ob = attach_offline(eng, sr, bs, 4);

        lifetime_policy timed{lifetime_kind::timed, 3, 1e-5, 0};
        instance_handle h = eng.spawn(sched, timed);
        ob->render(1);
        XR_CHECK(eng.active_count() == 1);

        ob->render(3);                 // ages past ttl=3 -> reaped
        XR_CHECK(eng.active_count() == 0);
        XR_CHECK(eng.reclaim() == 1);
        XR_CHECK(!eng.is_valid(h));
    }

    // --- until_finished reap ---
    XR_RUN("finished-flag auto-reap");
    {
        graph_blueprint bp = make_voice(440.0, 0.25);
        compiled_schedule sched = compile(bp, bs);

        engine eng;
        offline_backend* ob = attach_offline(eng, sr, bs, 4);

        instance_handle h = eng.spawn(sched, {lifetime_kind::until_finished, 0, 1e-5, 0});
        ob->render(1);
        XR_CHECK(eng.active_count() == 1);

        // Mark finished directly (single-threaded offline: safe here).
        eng.mgr.get(h)->finished_flag = true;
        ob->render(1);
        XR_CHECK(eng.active_count() == 0);
        XR_CHECK(eng.reclaim() == 1);
    }

    XR_MAIN_REPORT();
}
