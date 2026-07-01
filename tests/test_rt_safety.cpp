#include "core.hpp"
#include "standard_nodes.hpp"
#include "blueprint.hpp"
#include "schedule.hpp"
#include "instance.hpp"
#include "instance_manager.hpp"
#include "engine.hpp"
#include "offline_backend.hpp"
#include "rt_check.hpp"
#include "test_util.hpp"
#include <memory>
#include <new>
#include <cstdlib>

// Allocation trap: any allocation/free that happens while a thread is inside a
// no_alloc_scope (i.e. on the audio path, on the audio thread or a worker) is
// counted as a real-time violation. This is what proves the audio thread is
// allocation-free under stress.
void* operator new(std::size_t n) {
    if (xrune::rt::no_alloc_depth > 0)
        xrune::rt::alloc_violations.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept {
    if (xrune::rt::no_alloc_depth > 0)
        xrune::rt::alloc_violations.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

using namespace xrune;

static graph_blueprint make_voice(double freq) {
    graph_blueprint bp;
    size_t osc = bp.add<oscillator>(freq);
    size_t g = bp.add<gain>(0.1);
    bp.connect(osc, 0, g, 0);
    bp.set_output(g);
    return bp;
}

static graph_blueprint make_bus(double f) {
    graph_blueprint bp;
    size_t in = bp.add<bus_input>(1);
    size_t fad = bp.add<gain>(f);
    bp.connect(in, 0, fad, 0);
    bp.add_input_terminal("in", in);
    bp.set_output(fad);
    return bp;
}

int main() {
    const size_t sr = 48000, bs = 128, maxi = 64;

    // Self-test: prove the trap actually fires inside a no_alloc_scope, so a
    // passing result below is meaningful and not vacuous.
    XR_RUN("allocation trap fires");
    {
        xrune::rt::alloc_violations.store(0, std::memory_order_relaxed);
        { xrune::rt::no_alloc_scope s; volatile int* p = new int(7); delete p; }
        XR_CHECK(xrune::rt::alloc_violations.load(std::memory_order_relaxed) >= 1);
    }

    XR_RUN("audio thread is allocation-free under churn");

    graph_blueprint voice_bp = make_voice(220.0);   // must outlive the schedule
    compiled_schedule voice = compile(voice_bp, bs);
    graph_blueprint bus_bp = make_bus(0.5);
    compiled_schedule bus = compile(bus_bp, bs);

    engine eng;
    auto ob = std::make_unique<offline_backend>();
    offline_backend* obp = ob.get();
    eng.use_backend(std::move(ob));
    eng.init(sr, bs, 0, 2, maxi, /*num_workers=*/4);
    eng.start();

    // Control-thread setup (allocations here are fine, they're outside process()).
    instance_handle bh = eng.spawn(bus);
    std::vector<instance_handle> voices;
    for (int i = 0; i < 8; ++i) voices.push_back(eng.spawn(voice, {}, route_target::to(bh)));

    obp->render(8); // warm up: activate everything

    // Start counting only now; everything above ran on the control thread.
    xrune::rt::alloc_violations.store(0, std::memory_order_relaxed);

    // Stress: churn commands every block (all handled inside process()).
    uint64_t seed = 12345;
    auto rnd = [&]() { seed = seed * 6364136223846793005ULL + 1; return (seed >> 33); };
    for (int blk = 0; blk < 500; ++blk) {
        // parameter automation
        eng.set_parameter(voices[rnd() % voices.size()], 0, 0, 100.0 + (rnd() % 800));

        // occasional lifecycle + routing churn
        if (blk % 7 == 0) {
            size_t vi = rnd() % voices.size();
            eng.kill(voices[vi]);
            voices[vi] = eng.spawn(voice, {}, route_target::to(bh));
        }
        if (blk % 11 == 0) {
            size_t vi = rnd() % voices.size();
            eng.disconnect(voices[vi], 0, route_target::to(bh));
            eng.connect(voices[vi], 0, route_target::master());
        }

        obp->render(1);   // <-- audio-thread work happens here (must not allocate)
        eng.reclaim();    // control-thread free of reaped slots (outside process)
    }

    eng.stop();

    unsigned long long v = xrune::rt::alloc_violations.load(std::memory_order_relaxed);
    if (v != 0) std::cerr << "audio-thread allocation violations: " << v << "\n";
    XR_CHECK(v == 0);

    XR_MAIN_REPORT();
}
