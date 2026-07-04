#include "core.hpp"
#include "standard_nodes.hpp"
#include "blueprint.hpp"
#include "schedule.hpp"
#include "instance.hpp"
#include "test_util.hpp"
#include <memory>
#include <cmath>

using namespace xrune;

static constexpr double SR = 48000.0;
static constexpr size_t BS = 64;

int main() {
    // --- constant ---
    XR_RUN("constant");
    {
        graph_blueprint bp; size_t c = bp.add<constant>(0.7); bp.set_output(c);
        compiled_schedule s = compile(bp, BS); auto v = instantiate(s, (size_t)SR); v->process();
        auto o = v->output_view(0);
        for (size_t i = 0; i < o.size; ++i) XR_CHECK_NEAR(o[i], 0.7, 1e-12);
    }

    // --- gain ---
    XR_RUN("gain");
    {
        graph_blueprint bp; size_t c = bp.add<constant>(2.0); size_t g = bp.add<gain>(0.5);
        bp.connect(c, 0, g, 0); bp.set_output(g);
        compiled_schedule s = compile(bp, BS); auto v = instantiate(s, (size_t)SR); v->process();
        auto o = v->output_view(0);
        for (size_t i = 0; i < o.size; ++i) XR_CHECK_NEAR(o[i], 1.0, 1e-12);
    }

    // --- add / multiply ---
    XR_RUN("add / multiply");
    {
        graph_blueprint bp;
        size_t a = bp.add<constant>(0.3), b = bp.add<constant>(0.4);
        size_t s1 = bp.add<add>(), s2 = bp.add<multiply>();
        bp.connect(a, 0, s1, 0); bp.connect(b, 0, s1, 1);
        bp.connect(a, 0, s2, 0); bp.connect(b, 0, s2, 1);
        bp.set_output(s1);
        compiled_schedule s = compile(bp, BS); auto v = instantiate(s, (size_t)SR); v->process();
        XR_CHECK_NEAR(v->output_view(0)[0], 0.7, 1e-12);                 // add
        XR_CHECK_NEAR(v->node_output_view(s2, 0)[0], 0.12, 1e-12);       // multiply
    }

    // --- inverter ---
    XR_RUN("inverter");
    {
        graph_blueprint bp; size_t c = bp.add<constant>(0.5); size_t n = bp.add<inverter>();
        bp.connect(c, 0, n, 0); bp.set_output(n);
        compiled_schedule s = compile(bp, BS); auto v = instantiate(s, (size_t)SR); v->process();
        XR_CHECK_NEAR(v->output_view(0)[0], -0.5, 1e-12);
    }

    // --- mixer (sums N inputs) ---
    XR_RUN("mixer");
    {
        graph_blueprint bp;
        size_t a = bp.add<constant>(0.1), b = bp.add<constant>(0.2), c = bp.add<constant>(0.3);
        size_t m = bp.add<mixer>(3);
        bp.connect(a, 0, m, 0); bp.connect(b, 0, m, 1); bp.connect(c, 0, m, 2);
        bp.set_output(m);
        compiled_schedule s = compile(bp, BS); auto v = instantiate(s, (size_t)SR); v->process();
        XR_CHECK_NEAR(v->output_view(0)[0], 0.6, 1e-12);
    }

    // --- mono_to_stereo / stereo_to_mono ---
    XR_RUN("mono<->stereo");
    {
        graph_blueprint bp; size_t c = bp.add<constant>(0.5); size_t m = bp.add<mono_to_stereo>();
        bp.connect(c, 0, m, 0); bp.set_output(m);
        compiled_schedule s = compile(bp, BS); auto v = instantiate(s, (size_t)SR); v->process();
        XR_CHECK_NEAR(v->output_view(0)[0], 0.5, 1e-12);
        XR_CHECK_NEAR(v->output_view(1)[0], 0.5, 1e-12);
    }
    {
        graph_blueprint bp;
        size_t a = bp.add<constant>(0.4), b = bp.add<constant>(0.6);
        size_t m = bp.add<stereo_to_mono>();
        bp.connect(a, 0, m, 0); bp.connect(b, 0, m, 1); bp.set_output(m);
        compiled_schedule s = compile(bp, BS); auto v = instantiate(s, (size_t)SR); v->process();
        XR_CHECK_NEAR(v->output_view(0)[0], 0.5, 1e-12);
    }

    // --- pan (equal power; the bug fix) ---
    XR_RUN("pan equal-power");
    {
        auto pan_out = [&](double p, double& l, double& r) {
            graph_blueprint bp; size_t c = bp.add<constant>(1.0); size_t pn = bp.add<pan>(p);
            bp.connect(c, 0, pn, 0); bp.set_output(pn);
            compiled_schedule s = compile(bp, BS); auto v = instantiate(s, (size_t)SR); v->process();
            l = v->output_view(0)[0]; r = v->output_view(1)[0];
        };
        double l, r;
        pan_out(0.0, l, r);  XR_CHECK_NEAR(l, 0.70710678, 1e-6); XR_CHECK_NEAR(r, 0.70710678, 1e-6);
        pan_out(-1.0, l, r); XR_CHECK_NEAR(l, 1.0, 1e-6);        XR_CHECK_NEAR(r, 0.0, 1e-6);
        pan_out(1.0, l, r);  XR_CHECK_NEAR(l, 0.0, 1e-6);        XR_CHECK_NEAR(r, 1.0, 1e-6);
        // Equal power: l^2 + r^2 == 1 at every position.
        pan_out(0.3, l, r);  XR_CHECK_NEAR(l * l + r * r, 1.0, 1e-9);
    }

    // --- oscillator matches a sin phasor reference ---
    XR_RUN("oscillator");
    {
        graph_blueprint bp; size_t o = bp.add<oscillator>(1000.0); bp.set_output(o);
        compiled_schedule s = compile(bp, BS); auto v = instantiate(s, (size_t)SR); v->process();
        auto out = v->output_view(0);
        double phase = 0.0, inc = 2.0 * PI * 1000.0 / SR;
        for (size_t i = 0; i < out.size; ++i) {
            XR_CHECK_NEAR(out[i], std::sin(phase), 1e-12);
            phase += inc; if (phase >= 2.0 * PI) phase -= 2.0 * PI;
        }
    }

    // --- white noise statistics ---
    XR_RUN("white noise stats");
    {
        graph_blueprint bp; size_t n = bp.add<white_noise>(); bp.set_output(n);
        compiled_schedule s = compile(bp, BS); auto v = instantiate(s, (size_t)SR);
        double sum = 0, sumsq = 0; size_t count = 0; double peak = 0;
        for (int blk = 0; blk < 500; ++blk) {
            v->process(); auto o = v->output_view(0);
            for (size_t i = 0; i < o.size; ++i) { sum += o[i]; sumsq += o[i] * o[i]; peak = std::max(peak, std::fabs(o[i])); ++count; }
        }
        XR_CHECK_NEAR(sum / count, 0.0, 0.02);                 // zero mean
        XR_CHECK_NEAR(std::sqrt(sumsq / count), 0.57735, 0.02); // uniform[-1,1] std = 1/sqrt(3)
        XR_CHECK(peak <= 1.0);
    }

    // --- sample & hold (samples immediately, holds `interval` samples) ---
    XR_RUN("sample & hold");
    {
        const double rate = SR / 4.0;   // interval = 4 samples
        graph_blueprint bp;
        size_t o = bp.add<oscillator>(1000.0);
        size_t sh = bp.add<sample_and_hold>(rate);
        bp.connect(o, 0, sh, 0); bp.set_output(sh);
        compiled_schedule s = compile(bp, BS); auto v = instantiate(s, (size_t)SR); v->process();
        auto in = v->node_output_view(o, 0);
        auto out = v->output_view(0);
        for (size_t i = 0; i < out.size; ++i) {
            size_t sampled_at = (i / 4) * 4;                    // last sampling instant
            XR_CHECK_NEAR(out[i], in[sampled_at], 1e-12);       // held from that sample
        }
    }

    // --- channel adapter: sum (N->1) and fan-out (1->N) ---
    XR_RUN("channel adapter");
    {
        graph_blueprint bp;
        size_t a = bp.add<constant>(0.2), b = bp.add<constant>(0.5);
        size_t merge = bp.add<channel_adapter>(2, 1);           // sum
        bp.connect(a, 0, merge, 0); bp.connect(b, 0, merge, 1); bp.set_output(merge);
        compiled_schedule s = compile(bp, BS); auto v = instantiate(s, (size_t)SR); v->process();
        XR_CHECK_NEAR(v->output_view(0)[0], 0.7, 1e-12);
    }
    {
        graph_blueprint bp;
        size_t a = bp.add<constant>(0.9);
        size_t fan = bp.add<channel_adapter>(1, 3);             // fan-out
        bp.connect(a, 0, fan, 0); bp.set_output(fan);
        compiled_schedule s = compile(bp, BS); auto v = instantiate(s, (size_t)SR); v->process();
        for (size_t c = 0; c < 3; ++c) XR_CHECK_NEAR(v->output_view(c)[0], 0.9, 1e-12);
    }

    XR_MAIN_REPORT();
}
