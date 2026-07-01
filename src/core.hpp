#pragma once
#include <cstddef>

namespace xrune {

using sample_t = double;

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

    // Per-instance mutable DSP state (0 = stateless).
    virtual size_t state_size() const { return 0; }
    virtual size_t state_align() const { return alignof(sample_t); }
    virtual void init_state(void* state) const { (void)state; }

    virtual void process(void* state, const node_processing_context& ctx) const = 0;
};

} // namespace xrune
