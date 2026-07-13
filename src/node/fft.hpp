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
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstddef>

// Self-contained radix-2 FFT + an overlap-add STFT node (Tier-2). No external
// dependency; the FFT is abstracted behind fft_plan so a faster/SIMD backend can
// replace it later without touching the node. The STFT node demonstrates the
// Tier-2 contract from Phase 5: it runs at the host block rate, buffers frames
// internally, and presents output with latency.

namespace xrune {

// Immutable radix-2 FFT plan (power-of-two size). Precomputes the bit-reversal
// permutation and twiddle factors once; forward()/inverse() are const and
// operate in place on split real/imag arrays, so one plan is safely shared by
// every instance (and across threads).
struct fft_plan {
    size_t n = 0;
    std::vector<size_t> brev;
    std::vector<sample_t> tw_cos, tw_sin; // W_k = exp(-2πi k/n), k = 0..n/2-1

    void init(size_t size) {
        n = size;
        size_t levels = 0;
        while ((size_t(1) << levels) < n) ++levels;
        brev.resize(n);
        for (size_t i = 0; i < n; ++i) {
            size_t r = 0;
            for (size_t b = 0; b < levels; ++b)
                if (i & (size_t(1) << b)) r |= (size_t(1) << (levels - 1 - b));
            brev[i] = r;
        }
        tw_cos.resize(n / 2);
        tw_sin.resize(n / 2);
        for (size_t k = 0; k < n / 2; ++k) {
            tw_cos[k] = std::cos(2.0 * PI * static_cast<sample_t>(k) / static_cast<sample_t>(n));
            tw_sin[k] = std::sin(2.0 * PI * static_cast<sample_t>(k) / static_cast<sample_t>(n));
        }
    }

    void forward(sample_t* re, sample_t* im) const { transform(re, im, false); }
    void inverse(sample_t* re, sample_t* im) const { transform(re, im, true); }

private:
    void transform(sample_t* re, sample_t* im, bool inv) const {
        for (size_t i = 0; i < n; ++i) {
            size_t j = brev[i];
            if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        for (size_t len = 2; len <= n; len <<= 1) {
            const size_t half = len >> 1;
            const size_t step = n / len;
            for (size_t i = 0; i < n; i += len) {
                for (size_t j = 0; j < half; ++j) {
                    const size_t k = j * step;
                    const sample_t wr = tw_cos[k];
                    const sample_t wi = inv ? tw_sin[k] : -tw_sin[k];
                    const size_t a = i + j, b = a + half;
                    const sample_t tr = wr * re[b] - wi * im[b];
                    const sample_t ti = wr * im[b] + wi * re[b];
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr;        im[a] += ti;
                }
            }
        }
        if (inv) {
            const sample_t s = 1.0 / static_cast<sample_t>(n);
            for (size_t i = 0; i < n; ++i) { re[i] *= s; im[i] *= s; }
        }
    }
};

// ============================================================================
// OLA STFT  (Tier-2 spectral node). Runs at the host block rate; internally
// accumulates N-sample frames at 50% overlap, applies a Hann analysis window,
// FFT -> process_spectrum() -> IFFT, and overlap-adds the result. With the
// default identity spectral op it perfectly reconstructs the input delayed by
// N-1 samples (Hann at 50% hop is COLA with sum 1). Subclass and override
// process_spectrum() for spectral effects (filtering, freeze, ...).
//
// State is laid out manually in the instance's state block (its size depends on
// the FFT size, not the host block): [counter][in_ring:N][ola_ring:2N][re:N][im:N].
// ============================================================================
struct ola_stft : node {
    const char* type_name() const override { return "stft"; }
    size_t config_args(node_config_arg* out, size_t max) const override {
        if (max < 1) return 0;
        out[0] = {"size", static_cast<sample_t>(fft_size)};
        return 1;
    }
    size_t fft_size;
    size_t hop;
    fft_plan plan;                 // immutable, shared
    std::vector<sample_t> window;  // Hann analysis window, immutable

    explicit ola_stft(size_t n = 1024) : fft_size(n), hop(n / 2) {
        plan.init(n);
        window.resize(n);
        for (size_t i = 0; i < n; ++i)
            window[i] = 0.5 * (1.0 - std::cos(2.0 * PI * static_cast<sample_t>(i) / static_cast<sample_t>(n)));
    }

    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 1; }
    // Tier-2: host-rate node (1/1 rate, 1/1 block); latency is internal.

    struct counter { uint64_t t; };

    static size_t align_up(size_t x, size_t a) { return (x + a - 1) & ~(a - 1); }
    size_t arrays_offset() const { return align_up(sizeof(counter), alignof(sample_t)); }
    size_t state_size() const override {
        return arrays_offset() + (fft_size + 2 * fft_size + fft_size + fft_size) * sizeof(sample_t);
    }
    size_t state_align() const override {
        return alignof(counter) > alignof(sample_t) ? alignof(counter) : alignof(sample_t);
    }

    struct layout { counter* c; sample_t* in_ring; sample_t* ola; sample_t* re; sample_t* im; };
    layout map(void* s) const {
        auto* base = static_cast<std::byte*>(s);
        layout v;
        v.c = reinterpret_cast<counter*>(base);
        auto* p = reinterpret_cast<sample_t*>(base + arrays_offset());
        v.in_ring = p; p += fft_size;
        v.ola = p;     p += 2 * fft_size;
        v.re = p;      p += fft_size;
        v.im = p;      p += fft_size;
        return v;
    }

    void init_state(void* s) const override {
        layout v = map(s);
        v.c->t = 0;
        for (size_t i = 0; i < fft_size; ++i) { v.in_ring[i] = 0; v.re[i] = 0; v.im[i] = 0; }
        for (size_t i = 0; i < 2 * fft_size; ++i) v.ola[i] = 0;
    }

    // Spectral processing hook (default: identity). re/im hold N complex bins.
    virtual void process_spectrum(sample_t* re, sample_t* im, size_t n) const {
        (void)re; (void)im; (void)n;
    }

    void process(void* s, const node_processing_context& ctx) const override {
        layout v = map(s);
        const size_t N = fft_size, R = 2 * fft_size;
        for (size_t k = 0; k < ctx.block_size; ++k) {
            const uint64_t t = v.c->t;
            v.in_ring[t % N] = ctx.inputs[0][k];

            // Trigger a frame every `hop` samples once N samples are buffered.
            if (t >= N - 1 && ((t - (N - 1)) % hop == 0)) {
                const uint64_t start = t - (N - 1);
                for (size_t i = 0; i < N; ++i) {
                    v.re[i] = v.in_ring[(start + i) % N] * window[i];
                    v.im[i] = 0.0;
                }
                plan.forward(v.re, v.im);
                process_spectrum(v.re, v.im, N);
                plan.inverse(v.re, v.im);
                for (size_t i = 0; i < N; ++i)
                    v.ola[(start + i) % R] += v.re[i];
            }

            // Emit the output sample that is now fully overlapped (latency N-1).
            sample_t out = 0.0;
            if (t >= N - 1) {
                const size_t idx = static_cast<size_t>((t - (N - 1)) % R);
                out = v.ola[idx];
                v.ola[idx] = 0.0; // consume so the ring slot is clean for reuse
            }
            ctx.outputs[0][k] = out;
            v.c->t = t + 1;
        }
    }
};

// ============================================================================
// STFT split (block-rate). Exposes the spectrum on the graph so spectral nodes
// can process it, then resynthesizes. Unlike ola_stft this is *per block* (no
// overlap-add): fft_size must equal the engine block size, and a direct
// forward -> backward is exact (IFFT(FFT(x)) = x) with zero latency. Windowed
// overlap-add across the graph needs the block-size multi-rate region (pending),
// so these are honestly "stft", not "ola_stft".
//
//   stft_forward : `channels` real inputs  -> 2*channels outputs (re, im per ch)
//   stft_backward: 2*channels inputs (re,im) -> `channels` time outputs
// ============================================================================
struct stft_forward : node {
    const char* type_name() const override { return "stft_fwd"; }
    size_t config_args(node_config_arg* out, size_t max) const override {
        if (max < 2) return 0;
        out[0] = {"size", static_cast<sample_t>(fft_size)};
        out[1] = {"channels", static_cast<sample_t>(channels)};
        return 2;
    }
    size_t fft_size;
    size_t channels;
    fft_plan plan;

    explicit stft_forward(size_t n = 1024, size_t chans = 1)
        : fft_size(n), channels(chans) { plan.init(n); }

    size_t inputs_count() const override { return channels; }
    size_t outputs_count() const override { return channels * 2; }

    void process(void*, const node_processing_context& ctx) const override {
        if (ctx.block_size != fft_size) { // misconfigured: fail safe to silence
            for (size_t o = 0; o < outputs_count(); ++o)
                for (size_t i = 0; i < ctx.block_size; ++i) ctx.outputs[o][i] = 0.0;
            return;
        }
        for (size_t c = 0; c < channels; ++c) {
            sample_t* re = ctx.outputs[2 * c].data;      // FFT in place on the
            sample_t* im = ctx.outputs[2 * c + 1].data;  // output buffers
            const audio_buffer_view in = ctx.inputs[c];
            for (size_t i = 0; i < fft_size; ++i) { re[i] = in[i]; im[i] = 0.0; }
            plan.forward(re, im);
        }
    }
};

struct stft_backward : node {
    const char* type_name() const override { return "stft_bwd"; }
    size_t config_args(node_config_arg* out, size_t max) const override {
        if (max < 2) return 0;
        out[0] = {"size", static_cast<sample_t>(fft_size)};
        out[1] = {"channels", static_cast<sample_t>(channels)};
        return 2;
    }
    size_t fft_size;
    size_t channels;
    fft_plan plan;

    explicit stft_backward(size_t n = 1024, size_t chans = 1)
        : fft_size(n), channels(chans) { plan.init(n); }

    size_t inputs_count() const override { return channels * 2; }
    size_t outputs_count() const override { return channels; }
    // Per-channel imaginary scratch (IFFT is in place; inputs are shared buffers).
    size_t state_size() const override { return channels * fft_size * sizeof(sample_t); }
    size_t state_align() const override { return alignof(sample_t); }
    void init_state(void* s) const override {
        auto* p = static_cast<sample_t*>(s);
        for (size_t i = 0; i < channels * fft_size; ++i) p[i] = 0.0;
    }

    void process(void* s, const node_processing_context& ctx) const override {
        if (ctx.block_size != fft_size) {
            for (size_t o = 0; o < outputs_count(); ++o)
                for (size_t i = 0; i < ctx.block_size; ++i) ctx.outputs[o][i] = 0.0;
            return;
        }
        auto* scratch = static_cast<sample_t*>(s);
        for (size_t c = 0; c < channels; ++c) {
            const audio_buffer_view re_in = ctx.inputs[2 * c];
            const audio_buffer_view im_in = ctx.inputs[2 * c + 1];
            sample_t* out = ctx.outputs[c].data;
            sample_t* im = scratch + c * fft_size;
            for (size_t i = 0; i < fft_size; ++i) { out[i] = re_in[i]; im[i] = im_in[i]; }
            plan.inverse(out, im); // out now holds the real (time-domain) part
        }
    }
};

} // namespace xrune
