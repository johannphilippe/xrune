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
#include <cstddef>

namespace xrune {

using sample_t = double;

constexpr sample_t PI = 3.14159265358979323846;

// Alignment (bytes) for audio buffers and node state, chosen to satisfy every
// SIMD width: 64 is a multiple of 16 (SSE), 32 (AVX) and 64 (AVX-512), so an
// aligned pointer works for all of them, for float and double alike. With a
// power-of-two block size (enforced at engine init) every per-channel buffer
// then also starts on a 64-byte boundary.
constexpr size_t simd_align = 64;

struct audio_buffer_view {
    sample_t* data = nullptr;
    size_t size = 0;

    audio_buffer_view() = default;
    audio_buffer_view(sample_t* d, size_t s) : data(d), size(s) {}

    sample_t& operator[](size_t idx) const { return data[idx]; }
};

// Description of a control port (Phase 4). Exposed for introspection (Idyl):
// name for addressing, range for clamping, a type-level default (a node may
// override the effective default per blueprint via node::param_default).
struct port_descriptor {
    const char* name = "";
    sample_t default_value = 0.0;
    sample_t min_value = -1e30;
    sample_t max_value = 1e30;
};

// Hybrid-rate view of a control port handed to a node each block:
//  - audio-rate: `buffer` points at a connected source's per-sample output.
//  - control-rate: `buffer` is null; the value is a click-free linear ramp
//    (base + inc*i) from last block's value to the new target.
// Node code reads uniformly via at(i); a node that only needs a per-block value
// uses first().
struct param_view {
    const sample_t* buffer = nullptr;
    sample_t base = 0.0;
    sample_t inc = 0.0;

    sample_t at(size_t i) const {
        return buffer ? buffer[i] : base + inc * static_cast<sample_t>(i);
    }

    const sample_t* addr(size_t i) const  {
        return buffer + i;
    }

    sample_t first() const { return buffer ? buffer[0] : base; }
    bool is_audio_rate() const { return buffer != nullptr; }
};

// Buffers/params are supplied by the instance's scheduler as raw arrays (no
// per-block allocation). inputs are read-only signal streams; outputs are
// written; params are hybrid-rate control ports.
struct node_processing_context {
    const audio_buffer_view* inputs = nullptr;
    size_t input_count = 0;
    audio_buffer_view* outputs = nullptr;
    size_t output_count = 0;
    const param_view* params = nullptr;
    size_t param_count = 0;
    size_t sample_rate = 48000;
    size_t block_size = 128;
};

// One construction-time argument of a node ("inputs" = 4, "size" = 1024), as
// reported by node::config_args(). Serialization needs these to rebuild a node:
// params()/param_default() describe the *ports*, this describes the *constructor*.
// POD on purpose — no allocation, no includes.
struct node_config_arg {
    const char* name = "";
    sample_t value = 0.0;
};

// Node model (pre_roadmap §2, §5): a node type is *stateless code*. It describes
// its audio I/O, its control ports, and the size of a per-instance state block,
// and its process() operates on state + context supplied by the instance. Port
// *values* are framework-managed per instance (smoothing, connection override),
// so nodes no longer own or mutate parameter values — they only read them.
struct node {
    virtual ~node() = default;

    virtual size_t inputs_count() const = 0;
    virtual size_t outputs_count() const = 0;

    // Control ports.
    virtual size_t params_count() const { return 0; }
    virtual const port_descriptor* params() const { return nullptr; }
    // Effective default for a port (nodes with configurable defaults override).
    virtual sample_t param_default(size_t i) const {
        const port_descriptor* p = params();
        return p ? p[i].default_value : 0.0;
    }

    // ---- Introspection / serialization (control thread only) ----

    // The node's type name as registered in the node registry ("sine", "gain",
    // "mix", ...). The serializer writes it; the loader looks up the matching
    // factory. A node that returns the default is exportable but not loadable.
    virtual const char* type_name() const { return "node"; }

    // Construction-time configuration that is *not* a control port: a mixer's
    // input count, an FFT size. Ports are covered by params()/param_default();
    // these cover the constructor arguments, which are otherwise unrecoverable
    // from a node's public interface. Write up to `max` entries, return how many.
    virtual size_t config_args(node_config_arg* out, size_t max) const {
        (void)out; (void)max; return 0;
    }

    // Multi-rate (Phase 5): output *sample rate* as a ratio of the input rate.
    // Normal nodes are 1/1. An upsampler is 2/1, a downsampler 1/2. The compiler
    // propagates these along edges to give every node a region rate (a power of
    // two), and the scheduler calls the node that many times per cycle. A node
    // with a non-1/1 ratio is a rate boundary and must read/write using the
    // per-channel view sizes (which differ across the boundary), not block_size.
    virtual size_t rate_num() const { return 1; }
    virtual size_t rate_den() const { return 1; }

    // Output *block size* as a ratio of the input block size (factor of two:
    // 1/1, 2/1, or 1/2). A downbloc (1/2) makes its region run at a finer block
    // (more calls per cycle, same sample rate); an upbloc (2/1) runs coarser and
    // is only valid where the sample-rate region already supplies enough calls.
    virtual size_t block_num() const { return 1; }
    virtual size_t block_den() const { return 1; }

    // Per-instance mutable DSP state (0 = stateless).
    virtual size_t state_size() const { return 0; }
    virtual size_t state_align() const { return alignof(sample_t); }
    virtual void init_state(void* state) const { (void)state; }

    // Rate-aware setup, called on the control thread at spawn *after* init_state.
    // Nodes hosting a stateful sub-engine (Faust, Csound) that must be created
    // with the sample rate / block size do it here. Default: nothing.
    virtual void setup_state(void* state, size_t sample_rate, size_t block_size) const {
        (void)state; (void)sample_rate; (void)block_size;
    }
    // Teardown, called on the control thread when the instance is destroyed.
    // Release anything setup_state acquired (e.g. a hosted dsp instance).
    virtual void destroy_state(void* state) const { (void)state; }

    virtual void process(void* state, const node_processing_context& ctx) const = 0;
};

} // namespace xrune
