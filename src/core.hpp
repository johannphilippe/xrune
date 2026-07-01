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

// Buffer views are supplied by the instance's scheduler as raw arrays (no
// per-block allocation). inputs are read-only for the node; outputs are written.
struct node_processing_context {
    const audio_buffer_view* inputs = nullptr;
    size_t input_count = 0;
    audio_buffer_view* outputs = nullptr;
    size_t output_count = 0;
    size_t sample_rate = 48000;
    size_t block_size = 128;
};

// Node model (redesign decision Q1b, pre_roadmap §2): a node type is *stateless
// code*. It describes its I/O and the size/layout of a per-instance state block,
// and its process() operates on a state pointer supplied by the instance. All
// mutable, per-voice data lives in that state block (allocated from the arena),
// never in the node object itself — so one node object is safely shared by every
// instance of a blueprint.
struct node {
    virtual ~node() = default;

    virtual size_t inputs_count() const = 0;
    virtual size_t outputs_count() const = 0;

    // Per-instance mutable state layout. Default: stateless (0 bytes).
    virtual size_t state_size() const { return 0; }
    virtual size_t state_align() const { return alignof(sample_t); }

    // Initialize a freshly allocated per-instance state block. Runs on the
    // control thread at spawn. `state` is null iff state_size() == 0.
    virtual void init_state(void* state) const { (void)state; }

    // Process one block against this instance's state block.
    virtual void process(void* state, const node_processing_context& ctx) const = 0;

    // Apply a parameter change to a specific instance's state block.
    virtual void set_parameter(void* state, size_t index, sample_t value) const {
        (void)state; (void)index; (void)value;
    }
};

} // namespace xrune
