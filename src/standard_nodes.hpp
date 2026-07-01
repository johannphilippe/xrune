#pragma once
#include "core.hpp"
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace xrune {

constexpr sample_t PI = 3.14159265358979323846;

// ============================================================================
// OSCILLATOR  (state: frequency, phase; param 0 = frequency)
// ============================================================================
struct oscillator : node {
    struct st { sample_t frequency; sample_t phase; };
    sample_t default_freq = 440.0;

    oscillator() = default;
    explicit oscillator(sample_t freq) : default_freq(freq) {}

    size_t inputs_count() const override { return 0; }
    size_t outputs_count() const override { return 1; }
    size_t state_size() const override { return sizeof(st); }
    size_t state_align() const override { return alignof(st); }

    void init_state(void* s) const override {
        auto* x = static_cast<st*>(s);
        x->frequency = default_freq;
        x->phase = 0.0;
    }

    void process(void* s, const node_processing_context& ctx) const override {
        auto* x = static_cast<st*>(s);
        const sample_t inc = 2.0 * PI * x->frequency / static_cast<sample_t>(ctx.sample_rate);
        for (size_t i = 0; i < ctx.block_size; ++i) {
            ctx.outputs[0][i] = std::sin(x->phase);
            x->phase += inc;
            if (x->phase >= 2.0 * PI) x->phase -= 2.0 * PI;
        }
    }

    void set_parameter(void* s, size_t index, sample_t value) const override {
        if (index == 0) static_cast<st*>(s)->frequency = value;
    }
};

// ============================================================================
// GAIN  (state: value; param 0 = value)
// ============================================================================
struct gain : node {
    struct st { sample_t value; };
    sample_t default_value = 1.0;

    gain() = default;
    explicit gain(sample_t v) : default_value(v) {}

    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 1; }
    size_t state_size() const override { return sizeof(st); }
    size_t state_align() const override { return alignof(st); }

    void init_state(void* s) const override { static_cast<st*>(s)->value = default_value; }

    void process(void* s, const node_processing_context& ctx) const override {
        const sample_t v = static_cast<st*>(s)->value;
        for (size_t i = 0; i < ctx.block_size; ++i)
            ctx.outputs[0][i] = ctx.inputs[0][i] * v;
    }

    void set_parameter(void* s, size_t index, sample_t value) const override {
        if (index == 0) static_cast<st*>(s)->value = value;
    }
};

// ============================================================================
// MIXER  (stateless: sums all inputs to one output)
// ============================================================================
struct mixer : node {
    size_t in_count = 2;

    mixer() = default;
    explicit mixer(size_t ins) : in_count(ins) {}

    size_t inputs_count() const override { return in_count; }
    size_t outputs_count() const override { return 1; }

    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i) {
            sample_t sum = 0.0;
            for (size_t ch = 0; ch < in_count; ++ch) sum += ctx.inputs[ch][i];
            ctx.outputs[0][i] = sum;
        }
    }
};

// ============================================================================
// STEREO MIXER  (stateless: sums N stereo pairs to one stereo output)
// ============================================================================
struct stereo_mixer : node {
    size_t in_count = 2; // number of stereo pairs

    stereo_mixer() = default;
    explicit stereo_mixer(size_t ins) : in_count(ins) {}

    size_t inputs_count() const override { return in_count * 2; }
    size_t outputs_count() const override { return 2; }

    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i) {
            sample_t l = 0.0, r = 0.0;
            for (size_t ch = 0; ch < in_count; ++ch) {
                l += ctx.inputs[ch * 2][i];
                r += ctx.inputs[ch * 2 + 1][i];
            }
            ctx.outputs[0][i] = l;
            ctx.outputs[1][i] = r;
        }
    }
};

// ============================================================================
// STEREO FADER  (state: volume; param 0 = volume)
// ============================================================================
struct stereo_fader : node {
    struct st { sample_t volume; };
    sample_t default_volume = 1.0;

    stereo_fader() = default;
    explicit stereo_fader(sample_t vol) : default_volume(vol) {}

    size_t inputs_count() const override { return 2; }
    size_t outputs_count() const override { return 2; }
    size_t state_size() const override { return sizeof(st); }
    size_t state_align() const override { return alignof(st); }

    void init_state(void* s) const override { static_cast<st*>(s)->volume = default_volume; }

    void process(void* s, const node_processing_context& ctx) const override {
        const sample_t v = static_cast<st*>(s)->volume;
        for (size_t ch = 0; ch < 2; ++ch)
            for (size_t i = 0; i < ctx.block_size; ++i)
                ctx.outputs[ch][i] = ctx.inputs[ch][i] * v;
    }

    void set_parameter(void* s, size_t index, sample_t value) const override {
        if (index == 0) static_cast<st*>(s)->volume = value;
    }
};

// ============================================================================
// CHANNEL ADAPTER  (stateless: merge/split channels; writes full output block)
// ============================================================================
struct channel_adapter : node {
    size_t num_inputs;
    size_t num_outputs;

    channel_adapter(size_t inputs = 1, size_t outputs = 1)
        : num_inputs(inputs), num_outputs(outputs) {}

    size_t inputs_count() const override { return num_inputs; }
    size_t outputs_count() const override { return num_outputs; }

    void process(void*, const node_processing_context& ctx) const override {
        if (num_outputs == 1) { // sum all inputs
            for (size_t i = 0; i < ctx.block_size; ++i) {
                sample_t sum = 0.0;
                for (size_t ch = 0; ch < num_inputs; ++ch) sum += ctx.inputs[ch][i];
                ctx.outputs[0][i] = sum;
            }
        } else if (num_inputs == 1) { // fan out
            for (size_t ch = 0; ch < num_outputs; ++ch)
                for (size_t i = 0; i < ctx.block_size; ++i)
                    ctx.outputs[ch][i] = ctx.inputs[0][i];
        } else if (num_inputs > num_outputs && (num_inputs % num_outputs) == 0) { // merge
            for (size_t o = 0; o < num_outputs; ++o)
                for (size_t s = 0; s < ctx.block_size; ++s) {
                    sample_t sum = 0.0;
                    for (size_t i = o; i < num_inputs; i += num_outputs) sum += ctx.inputs[i][s];
                    ctx.outputs[o][s] = sum;
                }
        } else if (num_outputs > num_inputs && (num_outputs % num_inputs) == 0) { // split
            for (size_t o = 0; o < num_outputs; ++o) {
                size_t i = o % num_inputs;
                for (size_t s = 0; s < ctx.block_size; ++s) ctx.outputs[o][s] = ctx.inputs[i][s];
            }
        } else { // fallback: copy first input to all outputs
            for (size_t o = 0; o < num_outputs; ++o) {
                size_t i = std::min(o, num_inputs - 1);
                for (size_t s = 0; s < ctx.block_size; ++s) ctx.outputs[o][s] = ctx.inputs[i][s];
            }
        }
    }
};

// ============================================================================
// BUS INPUT  (stateless: entry point for an instance input terminal)
// Has no graph inputs; its output buffers are filled by the engine from routed
// upstream instances before the instance is processed, so process() is a no-op
// that simply preserves that externally-written signal for downstream nodes.
// ============================================================================
struct bus_input : node {
    size_t n;
    explicit bus_input(size_t channels = 2) : n(channels) {}
    size_t inputs_count() const override { return 0; }
    size_t outputs_count() const override { return n; }
    void process(void*, const node_processing_context&) const override {}
};

// ============================================================================
// MONO->STEREO / STEREO->MONO  (stateless)
// ============================================================================
struct mono_to_stereo : node {
    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 2; }
    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i) {
            sample_t m = ctx.inputs[0][i];
            ctx.outputs[0][i] = m;
            ctx.outputs[1][i] = m;
        }
    }
};

struct stereo_to_mono : node {
    size_t inputs_count() const override { return 2; }
    size_t outputs_count() const override { return 1; }
    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i)
            ctx.outputs[0][i] = (ctx.inputs[0][i] + ctx.inputs[1][i]) * 0.5;
    }
};

// ============================================================================
// PAN  (state: pan position; param 0 = pan in [-1, 1])
// ============================================================================
struct pan : node {
    struct st { sample_t pos; };
    sample_t default_pos = 0.0;

    pan() = default;
    explicit pan(sample_t p) : default_pos(p) {}

    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 2; }
    size_t state_size() const override { return sizeof(st); }
    size_t state_align() const override { return alignof(st); }

    void init_state(void* s) const override { static_cast<st*>(s)->pos = default_pos; }

    void process(void* s, const node_processing_context& ctx) const override {
        const sample_t p = std::clamp(static_cast<st*>(s)->pos, -1.0, 1.0);
        const sample_t angle = p * PI * 0.25;
        const sample_t lg = std::cos(angle), rg = std::sin(angle);
        for (size_t i = 0; i < ctx.block_size; ++i) {
            sample_t in = ctx.inputs[0][i];
            ctx.outputs[0][i] = in * lg;
            ctx.outputs[1][i] = in * rg;
        }
    }

    void set_parameter(void* s, size_t index, sample_t value) const override {
        if (index == 0) static_cast<st*>(s)->pos = value;
    }
};

// ============================================================================
// INVERTER / STEREO INVERTER / ADD / MULTIPLY  (stateless)
// ============================================================================
struct inverter : node {
    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 1; }
    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i) ctx.outputs[0][i] = -ctx.inputs[0][i];
    }
};

struct stereo_inverter : node {
    size_t inputs_count() const override { return 2; }
    size_t outputs_count() const override { return 2; }
    void process(void*, const node_processing_context& ctx) const override {
        for (size_t ch = 0; ch < 2; ++ch)
            for (size_t i = 0; i < ctx.block_size; ++i) ctx.outputs[ch][i] = -ctx.inputs[ch][i];
    }
};

struct add : node {
    size_t inputs_count() const override { return 2; }
    size_t outputs_count() const override { return 1; }
    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i)
            ctx.outputs[0][i] = ctx.inputs[0][i] + ctx.inputs[1][i];
    }
};

struct multiply : node {
    size_t inputs_count() const override { return 2; }
    size_t outputs_count() const override { return 1; }
    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i)
            ctx.outputs[0][i] = ctx.inputs[0][i] * ctx.inputs[1][i];
    }
};

// ============================================================================
// CONSTANT  (state: value; param 0 = value)
// ============================================================================
struct constant : node {
    struct st { sample_t value; };
    sample_t default_value = 1.0;

    constant() = default;
    explicit constant(sample_t v) : default_value(v) {}

    size_t inputs_count() const override { return 0; }
    size_t outputs_count() const override { return 1; }
    size_t state_size() const override { return sizeof(st); }
    size_t state_align() const override { return alignof(st); }

    void init_state(void* s) const override { static_cast<st*>(s)->value = default_value; }

    void process(void* s, const node_processing_context& ctx) const override {
        const sample_t v = static_cast<st*>(s)->value;
        for (size_t i = 0; i < ctx.block_size; ++i) ctx.outputs[0][i] = v;
    }

    void set_parameter(void* s, size_t index, sample_t value) const override {
        if (index == 0) static_cast<st*>(s)->value = value;
    }
};

// ============================================================================
// WHITE NOISE  (state: xorshift64* seed)
// ============================================================================
struct white_noise : node {
    struct st { uint64_t seed; };
    uint64_t default_seed = 0x853c49e6748fea9bULL;

    white_noise() = default;
    explicit white_noise(uint64_t s) : default_seed(s) {}

    size_t inputs_count() const override { return 0; }
    size_t outputs_count() const override { return 1; }
    size_t state_size() const override { return sizeof(st); }
    size_t state_align() const override { return alignof(st); }

    void init_state(void* s) const override { static_cast<st*>(s)->seed = default_seed; }

    void process(void* s, const node_processing_context& ctx) const override {
        uint64_t seed = static_cast<st*>(s)->seed;
        for (size_t i = 0; i < ctx.block_size; ++i) {
            seed ^= seed >> 12;
            seed ^= seed << 25;
            seed ^= seed >> 27;
            uint64_t r = seed * 0x2545F4914F6CDD1DULL;
            // Use the top 53 bits for an unbiased double in [0, 1).
            sample_t val = static_cast<sample_t>(r >> 11) * (1.0 / 9007199254740992.0);
            ctx.outputs[0][i] = val * 2.0 - 1.0;
        }
        static_cast<st*>(s)->seed = seed;
    }
};

// ============================================================================
// SAMPLE AND HOLD  (state: rate/held/counter; param 0 = rate in Hz)
// ============================================================================
struct sample_and_hold : node {
    struct st { sample_t rate_hz; sample_t held; size_t counter; };
    sample_t default_rate = 1.0;

    sample_and_hold() = default;
    explicit sample_and_hold(sample_t rate) : default_rate(rate) {}

    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 1; }
    size_t state_size() const override { return sizeof(st); }
    size_t state_align() const override { return alignof(st); }

    void init_state(void* s) const override {
        auto* x = static_cast<st*>(s);
        x->rate_hz = default_rate; x->held = 0.0; x->counter = 0;
    }

    void process(void* s, const node_processing_context& ctx) const override {
        auto* x = static_cast<st*>(s);
        if (x->rate_hz <= 0.0) {
            if (x->rate_hz < 0.0 && ctx.block_size > 0) x->held = ctx.inputs[0][0];
            for (size_t i = 0; i < ctx.block_size; ++i) ctx.outputs[0][i] = x->held;
            return;
        }
        size_t interval = static_cast<size_t>(static_cast<sample_t>(ctx.sample_rate) / x->rate_hz);
        if (interval == 0) interval = 1;
        for (size_t i = 0; i < ctx.block_size; ++i) {
            if (x->counter >= interval) { x->held = ctx.inputs[0][i]; x->counter = 0; }
            ctx.outputs[0][i] = x->held;
            ++x->counter;
        }
    }

    void set_parameter(void* s, size_t index, sample_t value) const override {
        if (index == 0) static_cast<st*>(s)->rate_hz = value;
    }
};

} // namespace xrune
