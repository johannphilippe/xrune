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
#include <vector>

using namespace xrune;

static constexpr size_t SR = 48000, BS = 128, BLOCKS = 64, MAXI = 128;

// A voice: osc(freq) -> gain(g) -> mono out. `depth` extra gains make it "heavy".
static graph_blueprint make_voice(double freq, double g, int depth) {
    graph_blueprint bp;
    size_t osc = bp.add<oscillator>(freq);
    size_t last = bp.add<gain>(g);
    bp.connect(osc, 0, last, 0);
    for (int i = 0; i < depth; ++i) {
        size_t nxt = bp.add<gain>(1.0);
        bp.connect(last, 0, nxt, 0);
        last = nxt;
    }
    bp.set_output(last);
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

// Render a scene with a given worker count; returns the interleaved output.
// variant: 0 = many voices->master, 1 = many voices->bus->master,
//          2 = few voices, 3 = few heavy voices.
static std::vector<double> render_scene(size_t num_workers, int variant) {
    size_t n_voices = (variant == 2 || variant == 3) ? 2 : 16;
    int depth = (variant == 3) ? 24 : 0;

    graph_blueprint voice_bp = make_voice(220.0, 0.05, depth);
    graph_blueprint bus_bp = make_bus(0.5);
    compiled_schedule voice = compile(voice_bp, BS);
    compiled_schedule bus = compile(bus_bp, BS);

    engine eng;
    auto ob = std::make_unique<offline_backend>();
    offline_backend* obp = ob.get();
    eng.use_backend(std::move(ob));
    eng.init(SR, BS, 0, 2, MAXI, num_workers);
    eng.start();

    instance_handle bh = null_handle;
    if (variant == 1) bh = eng.spawn(bus);
    for (size_t i = 0; i < n_voices; ++i) {
        // Distinct frequencies so voices are not identical.
        graph_blueprint* vbp = nullptr; (void)vbp;
        route_target dest = (variant == 1) ? route_target::to(bh) : route_target{};
        eng.spawn(voice, {}, dest);
    }

    obp->render(BLOCKS);
    eng.stop();
    return obp->output;
}

int main() {
    const char* names[] = {"many->master", "many->bus->master", "few voices", "few heavy voices"};
    for (int v = 0; v < 4; ++v) {
        XR_RUN(names[v]);
        std::vector<double> seq = render_scene(0, v);   // single-threaded
        std::vector<double> par = render_scene(4, v);   // 4 workers + audio thread

        XR_CHECK(seq.size() == par.size());
        // Produced actual sound (not silence).
        double e = 0.0; for (double x : seq) e += x * x;
        XR_CHECK(e > 0.0);

        // Parallel output must be bit-identical to sequential.
        bool identical = seq.size() == par.size();
        for (size_t i = 0; i < seq.size() && identical; ++i)
            if (seq[i] != par[i]) identical = false;
        XR_CHECK(identical);
    }

    XR_MAIN_REPORT();
}
