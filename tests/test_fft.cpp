#include "core.hpp"
#include "standard_nodes.hpp"
#include "node/fft.hpp"
#include "blueprint.hpp"
#include "schedule.hpp"
#include "instance.hpp"
#include "test_util.hpp"
#include <memory>
#include <vector>
#include <cmath>

using namespace xrune;

int main() {
    // --- FFT plan: impulse -> flat spectrum ---
    XR_RUN("fft impulse -> flat spectrum");
    {
        const size_t N = 64;
        fft_plan p; p.init(N);
        std::vector<sample_t> re(N, 0.0), im(N, 0.0);
        re[0] = 1.0;
        p.forward(re.data(), im.data());
        for (size_t k = 0; k < N; ++k) {
            XR_CHECK_NEAR(re[k], 1.0, 1e-12);
            XR_CHECK_NEAR(im[k], 0.0, 1e-12);
        }
    }

    // --- FFT plan: forward then inverse is identity ---
    XR_RUN("fft round-trip identity");
    {
        const size_t N = 256;
        fft_plan p; p.init(N);
        std::vector<sample_t> re(N), im(N, 0.0), ref(N);
        uint64_t seed = 99;
        auto rnd = [&]{ seed = seed*6364136223846793005ULL+1; return (double)(seed>>11)*(1.0/9007199254740992.0)*2.0-1.0; };
        for (size_t i = 0; i < N; ++i) { re[i] = rnd(); ref[i] = re[i]; }
        p.forward(re.data(), im.data());
        p.inverse(re.data(), im.data());
        for (size_t i = 0; i < N; ++i) XR_CHECK_NEAR(re[i], ref[i], 1e-10);
    }

    // --- FFT plan: pure bin -> single spectral line ---
    XR_RUN("fft single bin");
    {
        const size_t N = 64; const size_t bin = 5;
        fft_plan p; p.init(N);
        std::vector<sample_t> re(N), im(N, 0.0);
        for (size_t i = 0; i < N; ++i) re[i] = std::cos(2.0 * PI * bin * i / N);
        p.forward(re.data(), im.data());
        for (size_t k = 0; k < N; ++k) {
            double mag = std::sqrt(re[k]*re[k] + im[k]*im[k]);
            double expected = (k == bin || k == N - bin) ? (double)N / 2.0 : 0.0;
            XR_CHECK_NEAR(mag, expected, 1e-9);
        }
    }

    // --- OLA STFT perfect reconstruction (identity op) with N-1 latency ---
    XR_RUN("ola_stft reconstructs input (delayed)");
    {
        const size_t N = 64, bs = 32, sr = 48000;
        graph_blueprint bp;
        size_t osc = bp.add<oscillator>(1234.0);
        size_t st  = bp.add<ola_stft>(N);
        bp.connect(osc, 0, st, 0);
        bp.set_output(st);
        compiled_schedule s = compile(bp, bs);
        XR_CHECK(s.ok);
        auto v = instantiate(s, sr);

        std::vector<sample_t> in_stream, out_stream;
        for (int blk = 0; blk < 40; ++blk) {
            v->process();
            auto isrc = v->node_output_view(osc, 0);   // oscillator output (the reference)
            auto ostf = v->output_view(0);             // stft output
            for (size_t i = 0; i < isrc.size; ++i) { in_stream.push_back(isrc[i]); out_stream.push_back(ostf[i]); }
        }

        const size_t latency = N - 1;
        size_t checked = 0;
        // Skip warm-up (first ~2N output samples) then compare out[t] == in[t-latency].
        for (size_t t = 2 * N; t < out_stream.size(); ++t) {
            XR_CHECK_NEAR(out_stream[t], in_stream[t - latency], 1e-9);
            ++checked;
        }
        XR_CHECK(checked > 100);
    }

    // --- stft split: forward -> backward reconstructs exactly (zero latency) ---
    XR_RUN("stft split reconstructs");
    {
        const size_t N = 64, sr = 48000;
        graph_blueprint bp;
        size_t osc = bp.add<oscillator>(1234.0);
        size_t fwd = bp.add<stft_forward>(N, 1);
        size_t bwd = bp.add<stft_backward>(N, 1);
        bp.connect(osc, 0, fwd, 0);
        bp.connect(fwd, 0, bwd, 0); // real
        bp.connect(fwd, 1, bwd, 1); // imag
        bp.set_output(bwd);
        compiled_schedule s = compile(bp, N); // engine block == fft size
        XR_CHECK(s.ok);
        auto v = instantiate(s, sr);

        double maxerr = 0.0;
        for (int blk = 0; blk < 10; ++blk) {
            v->process();
            audio_buffer_view o = v->output_view(0);
            audio_buffer_view ref = v->node_output_view(osc, 0);
            for (size_t i = 0; i < o.size; ++i) maxerr = std::max(maxerr, std::fabs(o[i] - ref[i]));
        }
        XR_CHECK(maxerr < 1e-9); // IFFT(FFT(x)) == x
    }

    // --- spectral processing on the graph: scaling the spectrum scales time ---
    XR_RUN("stft split spectral scaling");
    {
        const size_t N = 64, sr = 48000;
        graph_blueprint bp;
        size_t osc = bp.add<oscillator>(1000.0);
        size_t fwd = bp.add<stft_forward>(N, 1);
        size_t gr  = bp.add<gain>(0.5);  // scale real bins
        size_t gi  = bp.add<gain>(0.5);  // scale imag bins
        size_t bwd = bp.add<stft_backward>(N, 1);
        bp.connect(osc, 0, fwd, 0);
        bp.connect(fwd, 0, gr, 0); bp.connect(gr, 0, bwd, 0);
        bp.connect(fwd, 1, gi, 0); bp.connect(gi, 0, bwd, 1);
        bp.set_output(bwd);
        compiled_schedule s = compile(bp, N);
        XR_CHECK(s.ok);
        auto v = instantiate(s, sr);

        double maxerr = 0.0;
        for (int blk = 0; blk < 10; ++blk) {
            v->process();
            audio_buffer_view o = v->output_view(0);
            audio_buffer_view ref = v->node_output_view(osc, 0);
            for (size_t i = 0; i < o.size; ++i) maxerr = std::max(maxerr, std::fabs(o[i] - 0.5 * ref[i]));
        }
        XR_CHECK(maxerr < 1e-9); // spectrum * 0.5 -> time * 0.5 (linearity)
    }

    XR_MAIN_REPORT();
}
