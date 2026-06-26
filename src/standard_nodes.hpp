#pragma once
#include "core.hpp"
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace xrune {

constexpr sample_t PI = 3.14159265358979323846;

// ============================================================================
// OSCILLATOR
// ============================================================================
struct oscillator : node {
    sample_t frequency = 440.0;
    sample_t phase = 0.0;

    oscillator() = default;
    oscillator(sample_t freq) : frequency(freq) {}

    size_t inputs_count() const override { return 0; }
    size_t outputs_count() const override { return 1; }

    void process(const node_processing_context& ctx) override {
        sample_t phase_increment = 2.0 * PI * frequency / ctx.sample_rate;
        for (size_t i = 0; i < ctx.block_size; ++i) {
            ctx.outputs[0][i] = std::sin(phase);
            phase += phase_increment;
            if (phase >= 2.0 * PI) {
                phase -= 2.0 * PI;
            }
        }
    }

    void set_parameter(size_t index, sample_t value) override {
        if (index == 0) {
            frequency = value;
        }
    }
};

// ============================================================================
// GAIN
// ============================================================================
struct gain : node {
    sample_t value = 1.0;

    gain() = default;
    gain(sample_t val) : value(val) {}

    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 1; }

    void process(const node_processing_context& ctx) override {
        for (size_t i = 0; i < ctx.block_size; ++i) {
            ctx.outputs[0][i] = ctx.inputs[0][i] * value;
        }
    }

    void set_parameter(size_t index, sample_t value) override {
        if (index == 0) {
            this->value = value;
        }
    }
};

// ============================================================================
// MIXER (multi-input, single output)
// Mixes multiple inputs into a single output by summing
// ============================================================================
struct mixer : node {
    size_t in_count = 2;

    mixer() = default;
    mixer(size_t ins) : in_count(ins) {}

    size_t inputs_count() const override { return in_count; }
    size_t outputs_count() const override { return 1; }

    void process(const node_processing_context& ctx) override {
        for (size_t i = 0; i < ctx.block_size; ++i) {
            sample_t sum = 0.0;
            for (size_t ch = 0; ch < in_count; ++ch) {
                sum += ctx.inputs[ch][i];
            }
            ctx.outputs[0][i] = sum;
        }
    }
};

// ============================================================================
// STEREO MIXER
// Mixes multiple stereo inputs into a single stereo output
// ============================================================================
struct stereo_mixer : node {
    size_t in_count = 2; // Number of stereo pairs to mix

    stereo_mixer() = default;
    stereo_mixer(size_t ins) : in_count(ins) {}

    size_t inputs_count() const override { return in_count * 2; }
    size_t outputs_count() const override { return 2; }

    void process(const node_processing_context& ctx) override {
        for (size_t i = 0; i < ctx.block_size; ++i) {
            sample_t left_sum = 0.0;
            sample_t right_sum = 0.0;
            for (size_t ch = 0; ch < in_count; ++ch) {
                left_sum += ctx.inputs[ch * 2][i];     // Left channel
                right_sum += ctx.inputs[ch * 2 + 1][i]; // Right channel
            }
            ctx.outputs[0][i] = left_sum;
            ctx.outputs[1][i] = right_sum;
        }
    }
};

// ============================================================================
// STEREO FADER
// ============================================================================
struct stereo_fader : node {
    sample_t volume = 1.0;

    stereo_fader() = default;
    stereo_fader(sample_t vol) : volume(vol) {}

    size_t inputs_count() const override { return 2; }
    size_t outputs_count() const override { return 2; }

    void process(const node_processing_context& ctx) override {
        for (size_t ch = 0; ch < 2; ++ch) {
            for (size_t i = 0; i < ctx.block_size; ++i) {
                ctx.outputs[ch][i] = ctx.inputs[ch][i] * volume;
            }
        }
    }

    void set_parameter(size_t index, sample_t value) override {
        if (index == 0) {
            volume = value;
        }
    }
};

// ============================================================================
// CHANNEL ADAPTER
// Merges or splits channels to enable connection between nodes with
// different numbers of inputs/outputs.
// Modes:
// - If 1 output, N inputs: sum all inputs to single output
// - If N outputs, 1 input: copy single input to all outputs
// - If N outputs > 1 and N inputs > 1:
//   * If inputs > outputs and inputs % outputs == 0: merge groups of inputs
//   * If outputs > inputs and outputs % inputs == 0: split input to outputs
// ============================================================================
struct channel_adapter : node {
    size_t num_inputs;
    size_t num_outputs;
    size_t process_count = 0;

    channel_adapter(size_t inputs = 1, size_t outputs = 1)
        : num_inputs(inputs), num_outputs(outputs) {}

    size_t inputs_count() const override { return num_inputs; }
    size_t outputs_count() const override { return num_outputs; }

    void process(const node_processing_context& ctx) override {
        // Clear outputs on first process call in a block
        if (process_count == 0) {
            for (size_t i = 0; i < num_outputs; ++i) {
                std::fill(ctx.outputs[i].data, ctx.outputs[i].data + ctx.block_size, 0.0);
            }
        }

        // Mode 1: Single output - sum all inputs
        if (num_outputs == 1) {
            for (size_t i = 0; i < ctx.block_size; ++i) {
                sample_t sum = 0.0;
                for (size_t ch = 0; ch < num_inputs; ++ch) {
                    sum += ctx.inputs[ch][i];
                }
                ctx.outputs[0][i] = sum;
            }
        }
        // Mode 2: Single input - copy to all outputs
        else if (num_inputs == 1) {
            for (size_t ch = 0; ch < num_outputs; ++ch) {
                for (size_t i = 0; i < ctx.block_size; ++i) {
                    ctx.outputs[ch][i] = ctx.inputs[0][i];
                }
            }
        }
        // Mode 3: Split or merge channels
        else {
            // If inputs > outputs and inputs is a multiple of outputs: merge
            if (num_inputs > num_outputs && (num_inputs % num_outputs) == 0) {
                for (size_t o = 0; o < num_outputs; ++o) {
                    for (size_t s = 0; s < ctx.block_size; ++s) {
                        sample_t sum = 0.0;
                        for (size_t i = o; i < num_inputs; i += num_outputs) {
                            sum += ctx.inputs[i][s];
                        }
                        ctx.outputs[o][s] = sum;
                    }
                }
            }
            // If outputs > inputs and outputs is a multiple of inputs: split
            else if (num_outputs > num_inputs && (num_outputs % num_inputs) == 0) {
                for (size_t o = 0; o < num_outputs; ++o) {
                    size_t i = o % num_inputs;
                    for (size_t s = 0; s < ctx.block_size; ++s) {
                        ctx.outputs[o][s] = ctx.inputs[i][s];
                    }
                }
            }
            // Default: copy first input to all outputs
            else {
                for (size_t o = 0; o < num_outputs; ++o) {
                    size_t input_ch = std::min(o, num_inputs - 1);
                    for (size_t s = 0; s < ctx.block_size; ++s) {
                        ctx.outputs[o][s] = ctx.inputs[input_ch][s];
                    }
                }
            }
        }

        process_count = (process_count + 1) % 1000; // Prevent overflow
    }
};

// ============================================================================
// MONO TO STEREO
// Simple adapter that copies mono input to both stereo outputs
// ============================================================================
struct mono_to_stereo : node {
    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 2; }

    void process(const node_processing_context& ctx) override {
        for (size_t i = 0; i < ctx.block_size; ++i) {
            sample_t mono = ctx.inputs[0][i];
            ctx.outputs[0][i] = mono;
            ctx.outputs[1][i] = mono;
        }
    }
};

// ============================================================================
// STEREO TO MONO
// Simple adapter that mixes stereo to mono (average of L+R)
// ============================================================================
struct stereo_to_mono : node {
    size_t inputs_count() const override { return 2; }
    size_t outputs_count() const override { return 1; }

    void process(const node_processing_context& ctx) override {
        for (size_t i = 0; i < ctx.block_size; ++i) {
            ctx.outputs[0][i] = (ctx.inputs[0][i] + ctx.inputs[1][i]) * 0.5;
        }
    }
};

// ============================================================================
// PAN (Stereo panner)
// Pans mono input between left and right outputs
// parameter 0: pan position (-1.0 = left, 0.0 = center, 1.0 = right)
// ============================================================================
struct pan : node {
    sample_t pan_position = 0.0; // -1.0 to 1.0

    pan() = default;
    pan(sample_t pan) : pan_position(pan) {}

    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 2; }

    void process(const node_processing_context& ctx) override {
        // Simple equal-power panning
        sample_t pan = std::clamp(pan_position, -1.0, 1.0);
        sample_t angle = pan * PI * 0.25; // -45 to +45 degrees
        sample_t left_gain = std::cos(angle);
        sample_t right_gain = std::sin(angle);

        for (size_t i = 0; i < ctx.block_size; ++i) {
            sample_t input = ctx.inputs[0][i];
            ctx.outputs[0][i] = input * left_gain;
            ctx.outputs[1][i] = input * right_gain;
        }
    }

    void set_parameter(size_t index, sample_t value) override {
        if (index == 0) {
            pan_position = value;
        }
    }
};

// ============================================================================
// INVERTER
// Inverts the signal (multiplies by -1)
// ============================================================================
struct inverter : node {
    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 1; }

    void process(const node_processing_context& ctx) override {
        for (size_t i = 0; i < ctx.block_size; ++i) {
            ctx.outputs[0][i] = -ctx.inputs[0][i];
        }
    }
};

// ============================================================================
// STEREO INVERTER
// Inverts both channels of a stereo signal
// ============================================================================
struct stereo_inverter : node {
    size_t inputs_count() const override { return 2; }
    size_t outputs_count() const override { return 2; }

    void process(const node_processing_context& ctx) override {
        for (size_t ch = 0; ch < 2; ++ch) {
            for (size_t i = 0; i < ctx.block_size; ++i) {
                ctx.outputs[ch][i] = -ctx.inputs[ch][i];
            }
        }
    }
};

// ============================================================================
// ADD
// Adds two signals together
// ============================================================================
struct add : node {
    size_t inputs_count() const override { return 2; }
    size_t outputs_count() const override { return 1; }

    void process(const node_processing_context& ctx) override {
        for (size_t i = 0; i < ctx.block_size; ++i) {
            ctx.outputs[0][i] = ctx.inputs[0][i] + ctx.inputs[1][i];
        }
    }
};

// ============================================================================
// MULTIPLY
// Multiplies two signals together (ring modulation, amplitude modulation)
// ============================================================================
struct multiply : node {
    size_t inputs_count() const override { return 2; }
    size_t outputs_count() const override { return 1; }

    void process(const node_processing_context& ctx) override {
        for (size_t i = 0; i < ctx.block_size; ++i) {
            ctx.outputs[0][i] = ctx.inputs[0][i] * ctx.inputs[1][i];
        }
    }
};

// ============================================================================
// CONSTANT
// Outputs a constant value
// ============================================================================
struct constant : node {
    sample_t value = 1.0;

    constant() = default;
    constant(sample_t val) : value(val) {}

    size_t inputs_count() const override { return 0; }
    size_t outputs_count() const override { return 1; }

    void process(const node_processing_context& ctx) override {
        for (size_t i = 0; i < ctx.block_size; ++i) {
            ctx.outputs[0][i] = value;
        }
    }

    void set_parameter(size_t index, sample_t val) override {
        if (index == 0) {
            value = val;
        }
    }
};

// ============================================================================
// WHITE NOISE
// Generates white noise
// ============================================================================
struct white_noise : node {
    uint64_t seed = 0x853c49e6748fea9bULL;

    white_noise() = default;
    white_noise(uint64_t s) : seed(s) {}

    size_t inputs_count() const override { return 0; }
    size_t outputs_count() const override { return 1; }

    // xorshift64* random number generator
    uint64_t next_random() {
        seed ^= seed >> 12;
        seed ^= seed << 25;
        seed ^= seed >> 27;
        return seed * 0x2545F4914F6CDD1DULL;
    }

    void process(const node_processing_context& ctx) override {
        for (size_t i = 0; i < ctx.block_size; ++i) {
            // Generate random float in range [-1.0, 1.0)
            uint64_t r = next_random();
            // Convert to double in range [0, 1)
            sample_t val = static_cast<sample_t>(r) / static_cast<sample_t>(0xFFFFFFFFFFFFFFFFULL);
            // Convert to [-1.0, 1.0)
            ctx.outputs[0][i] = val * 2.0 - 1.0;
        }
    }
};

// ============================================================================
// SAMPLE AND HOLD
// Samples the input at a given rate and holds the value
// parameter 0: sample rate in Hz (0 = pass through, negative = freeze)
// ============================================================================
struct sample_and_hold : node {
    sample_t sample_rate_hz = 1.0;
    sample_t held_value = 0.0;
    size_t sample_counter = 0;
    size_t sample_interval = 0;

    sample_and_hold() = default;
    sample_and_hold(sample_t rate) : sample_rate_hz(rate) {}

    size_t inputs_count() const override { return 1; }
    size_t outputs_count() const override { return 1; }

    void process(const node_processing_context& ctx) override {
        if (sample_rate_hz <= 0) {
            // Freeze mode - hold the current value
            if (sample_rate_hz < 0 && ctx.inputs[0].size > 0) {
                held_value = ctx.inputs[0][0];
            }
            for (size_t i = 0; i < ctx.block_size; ++i) {
                ctx.outputs[0][i] = held_value;
            }
        } else {
            // Calculate sample interval
            sample_interval = static_cast<size_t>(ctx.sample_rate / sample_rate_hz);
            if (sample_interval == 0) sample_interval = 1;

            for (size_t i = 0; i < ctx.block_size; ++i) {
                if (sample_counter >= sample_interval) {
                    held_value = ctx.inputs[0][i];
                    sample_counter = 0;
                }
                ctx.outputs[0][i] = held_value;
                sample_counter++;
            }
        }
    }

    void set_parameter(size_t index, sample_t value) override {
        if (index == 0) {
            sample_rate_hz = value;
        }
    }
};

} // namespace xrune
