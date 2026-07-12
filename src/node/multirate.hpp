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
#include "../core.hpp"
#include <cmath>
#include <algorithm>
#include <array>

// Multi-rate adapter nodes: sample-rate (upsampler2/downsampler2) and block-size
// (downbloc) boundaries, plus the shared half-band filter they use. Split out of
// standard_nodes.hpp because the DSP is non-trivial.

namespace xrune {

// ---------------------------------------------------------------------------
// Half-band low-pass FIR (cutoff at Fs/4) for 2x sample-rate conversion.
// Windowed-sinc, constructed as a true half-band: the center tap is exactly 0.5
// and the (nonzero) odd taps are scaled to sum to 0.5, so DC gain is 1 and the
// even/odd polyphase branches are balanced (no fs/2 ripple on constants).
// Even-offset taps are exactly zero (the half-band property).
// ---------------------------------------------------------------------------
constexpr int HB_N = 31;             // FIR length (odd)
constexpr int HB_CENTER = (HB_N - 1) / 2;

inline const std::array<sample_t, HB_N>& halfband_kernel() {
    static const std::array<sample_t, HB_N> h = []() {
        std::array<sample_t, HB_N> a{};
        sample_t odd_sum = 0.0;
        for (int n = 0; n < HB_N; ++n) {
            const int m = n - HB_CENTER;
            if (m == 0) { a[n] = 0.5; continue; }
            if (m % 2 == 0) { a[n] = 0.0; continue; }   // half-band zeros
            const sample_t x = PI * m / 2.0;
            const sample_t sinc = std::sin(x) / x;
            const sample_t w = 0.42 - 0.5 * std::cos(2.0 * PI * n / (HB_N - 1))
                                    + 0.08 * std::cos(4.0 * PI * n / (HB_N - 1));
            a[n] = 0.5 * sinc * w;
            odd_sum += a[n];
        }
        const sample_t scale = 0.5 / odd_sum; // force odd taps to sum to 0.5
        for (int n = 0; n < HB_N; ++n)
            if ((n - HB_CENTER) % 2 != 0) a[n] *= scale;
        return a;
    }();
    return h;
}

// Circular half-band delay line used by the up/down samplers.
struct halfband_state {
    sample_t z[HB_N];
    int pos;
};

inline void halfband_reset(halfband_state* s) {
    for (int i = 0; i < HB_N; ++i) s->z[i] = 0.0;
    s->pos = 0;
}

// Push one sample and return the filtered output (one MAC pass over the kernel).
inline sample_t halfband_step(halfband_state* s, sample_t in) {
    const auto& h = halfband_kernel();
    s->z[s->pos] = in;
    sample_t acc = 0.0;
    int idx = s->pos;
    for (int k = 0; k < HB_N; ++k) {
        acc += h[k] * s->z[idx];
        idx = (idx == 0) ? (HB_N - 1) : (idx - 1);
    }
    s->pos = (s->pos + 1) % HB_N;
    return acc;
}

// ============================================================================
// UPSAMPLER x2  (rate boundary 2/1): 1 input at rate R -> 1 output at rate 2R.
// Zero-stuff (insert a zero between input samples) then half-band low-pass at
// the new Fs/4, with x2 gain to compensate for the zero-stuffing. Reads B/2
// samples per call, writes B.
// ============================================================================
struct upsampler2 : node {
    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 1; }
    size_t rate_num() const override { return 2; }
    size_t rate_den() const override { return 1; }
    size_t state_size() const override { return sizeof(halfband_state); }
    size_t state_align() const override { return alignof(halfband_state); }
    void init_state(void* s) const override {
        halfband_reset(static_cast<halfband_state*>(s));
        halfband_kernel(); // force kernel init on the control thread
    }

    void process(void* s, const node_processing_context& ctx) const override {
        auto* hb = static_cast<halfband_state*>(s);
        const audio_buffer_view in = ctx.inputs[0];
        const audio_buffer_view out = ctx.outputs[0];
        for (size_t j = 0; j < in.size; ++j) {
            out[2 * j]     = 2.0 * halfband_step(hb, in[j]); // real sample
            out[2 * j + 1] = 2.0 * halfband_step(hb, 0.0);   // inserted zero
        }
    }
};

// ============================================================================
// DOWNSAMPLER /2  (rate boundary 1/2): 1 input at rate R -> 1 output at rate R/2.
// Half-band low-pass at the target Fs/4 first, then decimate (keep one sample of
// every two). Reads 2B samples per call, writes B.
// ============================================================================
struct downsampler2 : node {
    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 1; }
    size_t rate_num() const override { return 1; }
    size_t rate_den() const override { return 2; }
    size_t state_size() const override { return sizeof(halfband_state); }
    size_t state_align() const override { return alignof(halfband_state); }
    void init_state(void* s) const override {
        halfband_reset(static_cast<halfband_state*>(s));
        halfband_kernel();
    }

    void process(void* s, const node_processing_context& ctx) const override {
        auto* hb = static_cast<halfband_state*>(s);
        const audio_buffer_view in = ctx.inputs[0];
        const audio_buffer_view out = ctx.outputs[0];
        for (size_t i = 0; i < in.size; ++i) {
            const sample_t y = halfband_step(hb, in[i]); // filter every sample
            if ((i & 1) == 0) out[i / 2] = y;            // keep one of two
        }
    }
};

// ============================================================================
// DOWNBLOC  (block boundary 1/2): identity passthrough that makes its region
// run at a finer block (half the samples per call, twice as many calls per
// cycle) at the same sample rate. Throughput is unchanged, so it regroups the
// same samples into smaller blocks (e.g. for finer-grained control regions).
// The inverse (upbloc / block increase) is a cross-cycle case handled by
// internal-buffering Tier-2 host nodes (FFT, Csound) in a later phase.
// ============================================================================
struct downbloc : node {
    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 1; }
    size_t block_num() const override { return 1; }
    size_t block_den() const override { return 2; }
    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i) ctx.outputs[0][i] = ctx.inputs[0][i];
    }
};

} // namespace xrune
