#pragma once
#include "core.hpp"
#include <cmath>

namespace xrune {

constexpr sample_t PI = 3.14159265358979323846;

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

} // namespace xrune
