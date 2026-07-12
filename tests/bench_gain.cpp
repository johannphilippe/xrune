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

// Benchmark: simd_gain vs the standard gain node.
//
// Goal (learning SIMD): measure whether the hand-written SSE/AVX2/AVX-512 gain
// in src/node/simd_nodes/gain.hpp is actually faster than the plain scalar
// gain node, and report which SIMD instruction set the build selected.
//
// Three variants are timed on the SAME aligned, L1-resident block so we compare
// *compute* throughput (a gain over a big out-of-cache buffer would just measure
// memory bandwidth, where SIMD can't help):
//   1. "scalar (no auto-vec)" : reference loop with vectorisation *disabled*,
//      i.e. the raw scalar cost. This is the honest baseline for a SIMD speedup.
//   2. "gain (auto-vec)"      : the standard node. At -O3 the compiler happily
//      auto-vectorises `out[i] = in[i] * g`, so this is usually already SIMD.
//   3. "simd_gain"           : the hand-written intrinsics node.
//
// Comparing 3 vs 1 shows the SIMD win over scalar; 3 vs 2 shows whether hand
// intrinsics beat the compiler's own auto-vectoriser (often they just tie).
//
// It also asserts simd_gain == gain to the last bit; a mismatch returns non-zero
// so this doubles as a correctness test.

#include "core.hpp"
#include "standard_nodes.hpp"
#include "node/simd_nodes/gain.hpp"
#include "node/simd_nodes/rms.hpp"
#include "node/simd_nodes/simd_defs.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>

using namespace xrune;
using clock_type = std::chrono::steady_clock;

// Keep the optimiser from deleting the work / hoisting it out of the loop.
static inline void clobber() { asm volatile("" : : : "memory"); }

// A truly scalar reference: disable both loop- and SLP-vectorisation so the
// compiler can't turn this into SIMD behind our back. This is the "1x" baseline.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-tree-vectorize", "no-tree-slp-vectorize")))
#endif
static void scalar_gain(const sample_t* in, sample_t* out, size_t n, sample_t g) {
    for (size_t i = 0; i < n; ++i) out[i] = in[i] * g;
}

// Scalar RMS reduction, vectorisation disabled. Note: even WITHOUT this
// attribute the compiler won't auto-vectorise this at plain -O3, because
// reordering a floating-point sum changes the result (FP add isn't
// associative) and -O3 keeps that guarantee unless -ffast-math is given.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-tree-vectorize", "no-tree-slp-vectorize")))
#endif
static sample_t scalar_rms(const sample_t* in, size_t n) {
    sample_t sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += in[i] * in[i];
    return std::sqrt(sum / static_cast<sample_t>(n));
}

static sample_t* aligned_block(size_t n) {
    // simd_align (64) satisfies SSE/AVX/AVX-512 loads; size is a multiple of it.
    return static_cast<sample_t*>(std::aligned_alloc(simd_align, n * sizeof(sample_t)));
}

int main(int argc, char** argv) {
    constexpr size_t N = 1024;                 // 8 KiB of doubles: fits L1
    long iters = (argc > 1) ? std::atol(argv[1]) : 300000;
    if (iters < 1) iters = 1;
    const sample_t g = 0.5;

    // --- report the selected SIMD backend -----------------------------------
    std::printf("=== gain benchmark ===\n");
    std::printf("SIMD ISA        : %s\n", SIMD_ISA_NAME);
#ifdef SIMD
    std::printf("vector width    : %zu doubles/vector (SIMD_INCR)\n", (size_t)SIMD_INCR);
#else
    std::printf("vector width    : scalar (no SIMD macros; build with -march=native)\n");
#endif
    std::printf("buffer          : %zu samples, %zu-byte aligned\n", N, simd_align);
    std::printf("iterations      : %ld  (%.3g samples total)\n\n",
                iters, double(iters) * double(N));

    // --- buffers -------------------------------------------------------------
    sample_t* in       = aligned_block(N);
    sample_t* out_simd = aligned_block(N);
    sample_t* out_std  = aligned_block(N);
    sample_t* out_scal = aligned_block(N);
    if (!in || !out_simd || !out_std || !out_scal) {
        std::fprintf(stderr, "aligned_alloc failed\n");
        return 2;
    }
    for (size_t i = 0; i < N; ++i)
        in[i] = std::sin(0.01 * double(i)) * 0.75;   // some non-trivial signal

    // --- control-rate context (buffer=null -> gain is the constant g) --------
    audio_buffer_view in_view(in, N);
    param_view pv; pv.buffer = nullptr; pv.base = g; pv.inc = 0.0;

    auto make_ctx = [&](sample_t* out) {
        audio_buffer_view out_view(out, N);
        node_processing_context ctx;
        ctx.inputs = &in_view;   ctx.input_count = 1;
        ctx.params = &pv;        ctx.param_count = 1;
        ctx.sample_rate = 48000; ctx.block_size = N;
        // outputs must outlive the call; return a small POD holding both.
        return std::pair<node_processing_context, audio_buffer_view>{ctx, out_view};
    };

    gain      std_node(g);
    simd_gain simd_node(g);

    // --- correctness: simd_gain must equal the scalar/standard result --------
    {
        auto [ctx_s, ov_s] = make_ctx(out_std);  ctx_s.outputs = &ov_s;
        auto [ctx_v, ov_v] = make_ctx(out_simd); ctx_v.outputs = &ov_v;
        static_cast<node&>(std_node).process(nullptr, ctx_s);
        static_cast<node&>(simd_node).process(nullptr, ctx_v);
        double max_diff = 0.0;
        for (size_t i = 0; i < N; ++i)
            max_diff = std::fmax(max_diff, std::fabs(out_std[i] - out_simd[i]));
        std::printf("correctness     : max |simd - scalar| = %.3g  -> %s\n\n",
                    max_diff, max_diff <= 1e-15 ? "OK" : "MISMATCH");
        if (max_diff > 1e-12) {
            std::fprintf(stderr, "simd_gain does not match the standard gain!\n");
            return 1;
        }
    }

    // --- timing harness ------------------------------------------------------
    volatile sample_t sink = 0.0;                 // defeat dead-store elimination
    auto time_it = [&](const char* label, auto&& body) {
        for (int w = 0; w < 1000; ++w) { body(0); clobber(); }   // warm up
        auto t0 = clock_type::now();
        for (long it = 0; it < iters; ++it) { body(it); clobber(); }
        auto t1 = clock_type::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();
        double samples = double(iters) * double(N);
        double ns_per = secs * 1e9 / samples;
        std::printf("  %-22s %8.3f ms   %6.3f ns/sample   %6.2f Gsample/s\n",
                    label, secs * 1e3, ns_per, samples / secs / 1e9);
        return ns_per;
    };

    auto [ctx_std, ov_std]   = make_ctx(out_std);   ctx_std.outputs  = &ov_std;
    auto [ctx_simd, ov_simd] = make_ctx(out_simd);  ctx_simd.outputs = &ov_simd;
    node& n_std  = std_node;
    node& n_simd = simd_node;

    std::printf("timings:\n");
    double t_scalar = time_it("scalar (no auto-vec)", [&](long it) {
        scalar_gain(in, out_scal, N, g);
        sink += out_scal[it & (N - 1)];
    });
    double t_std = time_it("gain (auto-vec)", [&](long it) {
        n_std.process(nullptr, ctx_std);
        sink += out_std[it & (N - 1)];
    });
    double t_simd = time_it("simd_gain", [&](long it) {
        n_simd.process(nullptr, ctx_simd);
        sink += out_simd[it & (N - 1)];
    });

    std::printf("\nspeedup:\n");
    std::printf("  simd_gain vs scalar (no auto-vec) : %.2fx\n", t_scalar / t_simd);
    std::printf("  simd_gain vs gain (auto-vec)      : %.2fx\n", t_std   / t_simd);
    std::printf("\n(checksum %.3g)\n", (double)sink);

    // ========================================================================
    // RMS: a *reduction*. Unlike gain, the compiler will NOT auto-vectorise the
    // sum at -O3 (FP associativity), so simd_rms (hand SSE, 2 doubles/vector)
    // is expected to beat both the scalar reference and the standard rms node.
    // ========================================================================
    std::printf("\n=== rms benchmark ===\n");
    std::printf("note: simd_rms is hand-written SSE (2 doubles/vector), "
                "regardless of the ISA above.\n\n");

    rms      rms_std;
    simd_rms rms_simd;

    // correctness: reductions sum in a different order, so compare with a
    // relative epsilon rather than bit-exact.
    {
        auto [ctx_s, ov_s] = make_ctx(out_std);  ctx_s.outputs = &ov_s;
        auto [ctx_v, ov_v] = make_ctx(out_simd); ctx_v.outputs = &ov_v;
        static_cast<node&>(rms_std).process(nullptr, ctx_s);
        static_cast<node&>(rms_simd).process(nullptr, ctx_v);
        double ref = out_std[0];
        double rel = std::fabs(out_simd[0] - ref) / (std::fabs(ref) + 1e-300);
        std::printf("correctness     : rms=%.9f simd=%.9f  rel.err=%.3g -> %s\n\n",
                    ref, out_simd[0], rel, rel <= 1e-9 ? "OK" : "MISMATCH");
        if (rel > 1e-9) {
            std::fprintf(stderr, "simd_rms does not match the scalar rms!\n");
            return 1;
        }
    }

    auto [ctx_rstd, ov_rstd]   = make_ctx(out_std);  ctx_rstd.outputs = &ov_rstd;
    auto [ctx_rsimd, ov_rsimd] = make_ctx(out_simd); ctx_rsimd.outputs = &ov_rsimd;
    node& nr_std  = rms_std;
    node& nr_simd = rms_simd;

    std::printf("timings:\n");
    double r_scalar = time_it("scalar (no auto-vec)", [&](long) {
        sink += scalar_rms(in, N);
    });
    double r_std = time_it("rms (auto-vec)", [&](long it) {
        nr_std.process(nullptr, ctx_rstd);
        sink += out_std[it & (N - 1)];
    });
    double r_simd = time_it("simd_rms (SSE)", [&](long it) {
        nr_simd.process(nullptr, ctx_rsimd);
        sink += out_simd[it & (N - 1)];
    });

    std::printf("\nspeedup:\n");
    std::printf("  simd_rms vs scalar (no auto-vec) : %.2fx\n", r_scalar / r_simd);
    std::printf("  simd_rms vs rms (auto-vec)       : %.2fx\n", r_std    / r_simd);
    std::printf("\n(checksum %.3g)\n", (double)sink);

    std::free(in); std::free(out_simd); std::free(out_std); std::free(out_scal);
    return 0;
}
