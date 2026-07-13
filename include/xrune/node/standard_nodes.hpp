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
#include "xrune/core.hpp"
#include "xrune/node/multirate.hpp"   // upsampler2, downsampler2, downbloc, halfband
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace xrune {

// ============================================================================
// OSCILLATOR  (port: freq; state: phase). freq is modulatable (FM/vibrato).
// ============================================================================
struct oscillator : node {
    const char* type_name() const override { return "sine"; }
    struct st { sample_t phase; };
    static constexpr port_descriptor PORTS[] = {{"freq", 440.0, 0.0, 20000.0}};
    sample_t default_freq = 440.0;

    oscillator() = default;
    explicit oscillator(sample_t freq) : default_freq(freq) {}

    size_t inputs_count() const override { return 0; }
    size_t outputs_count() const override { return 1; }
    size_t params_count() const override { return 1; }
    const port_descriptor* params() const override { return PORTS; }
    sample_t param_default(size_t i) const override { return i == 0 ? default_freq : 0.0; }
    size_t state_size() const override { return sizeof(st); }
    size_t state_align() const override { return alignof(st); }

    void init_state(void* s) const override { static_cast<st*>(s)->phase = 0.0; }

    void process(void* s, const node_processing_context& ctx) const override {
        auto* x = static_cast<st*>(s);
        const sample_t k = 2.0 * PI / static_cast<sample_t>(ctx.sample_rate);
        for (size_t i = 0; i < ctx.block_size; ++i) {
            ctx.outputs[0][i] = std::sin(x->phase);
            x->phase += k * ctx.params[0].at(i);
            if (x->phase >= 2.0 * PI) x->phase -= 2.0 * PI;
            else if (x->phase < 0.0) x->phase += 2.0 * PI;
        }
    }
};

// ============================================================================
// GAIN  (port: gain; stateless)
// ============================================================================
struct gain : node {
    const char* type_name() const override { return "gain"; }
    static constexpr port_descriptor PORTS[] = {{"gain", 1.0, -1e30, 1e30}};
    sample_t default_value = 1.0;

    gain() = default;
    explicit gain(sample_t v) : default_value(v) {}

    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 1; }
    size_t params_count() const override { return 1; }
    const port_descriptor* params() const override { return PORTS; }
    sample_t param_default(size_t i) const override { return i == 0 ? default_value : 1.0; }

    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i)
            ctx.outputs[0][i] = ctx.inputs[0][i] * ctx.params[0].at(i);
    }
};

// ============================================================================
// MIXER / STEREO MIXER  (stateless; no ports)
// ============================================================================
struct mixer : node {
    const char* type_name() const override { return "mix"; }
    size_t config_args(node_config_arg* out, size_t max) const override {
        if (max < 1) return 0;
        out[0] = {"inputs", static_cast<sample_t>(in_count)};
        return 1;
    }
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

struct stereo_mixer : node {
    const char* type_name() const override { return "smix"; }
    size_t config_args(node_config_arg* out, size_t max) const override {
        if (max < 1) return 0;
        out[0] = {"inputs", static_cast<sample_t>(in_count)};
        return 1;
    }
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
// STEREO FADER  (port: volume; stateless)
// ============================================================================
struct stereo_fader : node {
    const char* type_name() const override { return "fader"; }
    static constexpr port_descriptor PORTS[] = {{"volume", 1.0, 0.0, 1e30}};
    sample_t default_volume = 1.0;

    stereo_fader() = default;
    explicit stereo_fader(sample_t vol) : default_volume(vol) {}

    size_t inputs_count() const override { return 2; }
    size_t outputs_count() const override { return 2; }
    size_t params_count() const override { return 1; }
    const port_descriptor* params() const override { return PORTS; }
    sample_t param_default(size_t i) const override { return i == 0 ? default_volume : 1.0; }

    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i) {
            const sample_t v = ctx.params[0].at(i);
            ctx.outputs[0][i] = ctx.inputs[0][i] * v;
            ctx.outputs[1][i] = ctx.inputs[1][i] * v;
        }
    }
};

// ============================================================================
// BUS INPUT  (stateless; no ports) - engine fills its outputs from routes.
// ============================================================================
struct bus_input : node {
    const char* type_name() const override { return "bus"; }
    size_t config_args(node_config_arg* out, size_t max) const override {
        if (max < 1) return 0;
        out[0] = {"channels", static_cast<sample_t>(n)};
        return 1;
    }
    size_t n;
    explicit bus_input(size_t channels = 2) : n(channels) {}
    size_t inputs_count() const override { return 0; }
    size_t outputs_count() const override { return n; }
    void process(void*, const node_processing_context&) const override {}
};

// ============================================================================
// CHANNEL ADAPTER  (stateless; no ports)
// ============================================================================
struct channel_adapter : node {
    const char* type_name() const override { return "adapt"; }
    size_t config_args(node_config_arg* out, size_t max) const override {
        if (max < 2) return 0;
        out[0] = {"inputs", static_cast<sample_t>(num_inputs)};
        out[1] = {"outputs", static_cast<sample_t>(num_outputs)};
        return 2;
    }
    size_t num_inputs;
    size_t num_outputs;
    channel_adapter(size_t inputs = 1, size_t outputs = 1)
        : num_inputs(inputs), num_outputs(outputs) {}
    size_t inputs_count() const override { return num_inputs; }
    size_t outputs_count() const override { return num_outputs; }
    void process(void*, const node_processing_context& ctx) const override {
        if (num_outputs == 1) {
            for (size_t i = 0; i < ctx.block_size; ++i) {
                sample_t sum = 0.0;
                for (size_t ch = 0; ch < num_inputs; ++ch) sum += ctx.inputs[ch][i];
                ctx.outputs[0][i] = sum;
            }
        } else if (num_inputs == 1) {
            for (size_t ch = 0; ch < num_outputs; ++ch)
                for (size_t i = 0; i < ctx.block_size; ++i) ctx.outputs[ch][i] = ctx.inputs[0][i];
        } else if (num_inputs > num_outputs && (num_inputs % num_outputs) == 0) {
            for (size_t o = 0; o < num_outputs; ++o)
                for (size_t s = 0; s < ctx.block_size; ++s) {
                    sample_t sum = 0.0;
                    for (size_t i = o; i < num_inputs; i += num_outputs) sum += ctx.inputs[i][s];
                    ctx.outputs[o][s] = sum;
                }
        } else if (num_outputs > num_inputs && (num_outputs % num_inputs) == 0) {
            for (size_t o = 0; o < num_outputs; ++o) {
                size_t i = o % num_inputs;
                for (size_t s = 0; s < ctx.block_size; ++s) ctx.outputs[o][s] = ctx.inputs[i][s];
            }
        } else {
            for (size_t o = 0; o < num_outputs; ++o) {
                size_t i = std::min(o, num_inputs - 1);
                for (size_t s = 0; s < ctx.block_size; ++s) ctx.outputs[o][s] = ctx.inputs[i][s];
            }
        }
    }
};

// ============================================================================
// MONO<->STEREO  (stateless; no ports)
// ============================================================================
struct mono_to_stereo : node {
    const char* type_name() const override { return "m2s"; }
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
    const char* type_name() const override { return "s2m"; }
    size_t inputs_count() const override { return 2; }
    size_t outputs_count() const override { return 1; }
    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i)
            ctx.outputs[0][i] = (ctx.inputs[0][i] + ctx.inputs[1][i]) * 0.5;
    }
};

// ============================================================================
// PAN  (port: pan in [-1,1]; stateless). Equal-power; pan is modulatable.
// ============================================================================
struct pan : node {
    const char* type_name() const override { return "pan"; }
    static constexpr port_descriptor PORTS[] = {{"pan", 0.0, -1.0, 1.0}};
    sample_t default_pos = 0.0;

    pan() = default;
    explicit pan(sample_t p) : default_pos(p) {}

    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 2; }
    size_t params_count() const override { return 1; }
    const port_descriptor* params() const override { return PORTS; }
    sample_t param_default(size_t i) const override { return i == 0 ? default_pos : 0.0; }

    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i) {
            const sample_t p = std::clamp(ctx.params[0].at(i), -1.0, 1.0);
            // Map [-1,1] -> [0, PI/2] so center (0) is equal power (both cos/sin
            // of PI/4 = 0.707), -1 is hard left, +1 is hard right.
            const sample_t angle = (p + 1.0) * PI * 0.25;
            const sample_t in = ctx.inputs[0][i];
            ctx.outputs[0][i] = in * std::cos(angle);
            ctx.outputs[1][i] = in * std::sin(angle);
        }
    }
};

// ============================================================================
// INVERTER / STEREO INVERTER / ADD / MULTIPLY  (stateless; no ports)
// ============================================================================
struct inverter : node {
    const char* type_name() const override { return "inv"; }
    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 1; }
    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i) ctx.outputs[0][i] = -ctx.inputs[0][i];
    }
};

struct stereo_inverter : node {
    const char* type_name() const override { return "sinv"; }
    size_t inputs_count() const override { return 2; }
    size_t outputs_count() const override { return 2; }
    void process(void*, const node_processing_context& ctx) const override {
        for (size_t ch = 0; ch < 2; ++ch)
            for (size_t i = 0; i < ctx.block_size; ++i) ctx.outputs[ch][i] = -ctx.inputs[ch][i];
    }
};

struct add : node {
    const char* type_name() const override { return "add"; }
    size_t inputs_count() const override { return 2; }
    size_t outputs_count() const override { return 1; }
    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i)
            ctx.outputs[0][i] = ctx.inputs[0][i] + ctx.inputs[1][i];
    }
};

struct multiply : node {
    const char* type_name() const override { return "mul"; }
    size_t inputs_count() const override { return 2; }
    size_t outputs_count() const override { return 1; }
    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i)
            ctx.outputs[0][i] = ctx.inputs[0][i] * ctx.inputs[1][i];
    }
};

// ============================================================================
// CONSTANT  (port: value; stateless). A trivial control source: its output IS
// its (modulatable) port value.
// ============================================================================
struct constant : node {
    const char* type_name() const override { return "constant"; }
    static constexpr port_descriptor PORTS[] = {{"value", 1.0, -1e30, 1e30}};
    sample_t default_value = 1.0;

    constant() = default;
    explicit constant(sample_t v) : default_value(v) {}

    size_t inputs_count() const override { return 0; }
    size_t outputs_count() const override { return 1; }
    size_t params_count() const override { return 1; }
    const port_descriptor* params() const override { return PORTS; }
    sample_t param_default(size_t i) const override { return i == 0 ? default_value : 0.0; }

    void process(void*, const node_processing_context& ctx) const override {
        for (size_t i = 0; i < ctx.block_size; ++i) ctx.outputs[0][i] = ctx.params[0].at(i);
    }
};

// ============================================================================
// WHITE NOISE  (state: xorshift64* seed; no ports; seed is blueprint config)
// ============================================================================
struct white_noise : node {
    const char* type_name() const override { return "noise"; }
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
            sample_t val = static_cast<sample_t>(r >> 11) * (1.0 / 9007199254740992.0);
            ctx.outputs[0][i] = val * 2.0 - 1.0;
        }
        static_cast<st*>(s)->seed = seed;
    }
};

// ============================================================================
// CALL COUNTER  (passthrough that counts process() calls; test/introspection)
// ============================================================================
struct call_counter : node {
    const char* type_name() const override { return "counter"; }
    struct st { size_t calls; };
    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 1; }
    size_t state_size() const override { return sizeof(st); }
    size_t state_align() const override { return alignof(st); }
    void init_state(void* s) const override { static_cast<st*>(s)->calls = 0; }

    void process(void* s, const node_processing_context& ctx) const override {
        static_cast<st*>(s)->calls++;
        for (size_t i = 0; i < ctx.block_size; ++i) ctx.outputs[0][i] = ctx.inputs[0][i];
    }
};

// ============================================================================
// SAMPLE AND HOLD  (port: rate Hz; state: held/counter). rate read per block.
// ============================================================================
struct sample_and_hold : node {
    const char* type_name() const override { return "sah"; }
    struct st { sample_t held; size_t counter; };
    static constexpr port_descriptor PORTS[] = {{"rate", 1.0, -1e30, 1e30}};
    sample_t default_rate = 1.0;

    sample_and_hold() = default;
    explicit sample_and_hold(sample_t rate) : default_rate(rate) {}

    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 1; }
    size_t params_count() const override { return 1; }
    const port_descriptor* params() const override { return PORTS; }
    sample_t param_default(size_t i) const override { return i == 0 ? default_rate : 1.0; }
    size_t state_size() const override { return sizeof(st); }
    size_t state_align() const override { return alignof(st); }

    void init_state(void* s) const override {
        auto* x = static_cast<st*>(s);
        x->held = 0.0; x->counter = 0;
    }

    void process(void* s, const node_processing_context& ctx) const override {
        auto* x = static_cast<st*>(s);
        const sample_t rate = ctx.params[0].first();
        if (rate <= 0.0) {
            if (rate < 0.0 && ctx.block_size > 0) x->held = ctx.inputs[0][0];
            for (size_t i = 0; i < ctx.block_size; ++i) ctx.outputs[0][i] = x->held;
            return;
        }
        size_t interval = static_cast<size_t>(static_cast<sample_t>(ctx.sample_rate) / rate);
        if (interval == 0) interval = 1;
        for (size_t i = 0; i < ctx.block_size; ++i) {
            // Sample the first input immediately (counter starts at 0), then
            // re-sample every `interval` samples.
            if (x->counter == 0) x->held = ctx.inputs[0][i];
            ctx.outputs[0][i] = x->held;
            if (++x->counter >= interval) x->counter = 0;
        }
    }
};

} // namespace xrune
