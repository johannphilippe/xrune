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
#include "xrune/audio/backend.hpp"
#include <vector>
#include <cmath>
#include <algorithm>

namespace xrune {

// Headless backend: instead of a device, it renders a fixed number of blocks
// synchronously into an in-memory interleaved buffer. This is what makes the
// engine testable on CI without a sound card.
//
// Usage: open() (via engine.init), then call render(num_blocks) on the control
// thread and inspect `output` / rms() / peak().
struct offline_backend : audio_backend {
    backend_config cfg;
    audio_process_fn fn;

    // Interleaved output captured by the most recent render(): size =
    // num_blocks * block_size * output_channels.
    std::vector<double> output;
    bool running = false;

    bool open(const backend_config& c, audio_process_fn f) override {
        cfg = c;
        fn = std::move(f);
        return true;
    }

    bool start() override { running = true; return true; }
    void stop() override { running = false; }
    void close() override {}

    // Drive the DSP callback `num_blocks` times, capturing interleaved output.
    void render(size_t num_blocks) {
        const size_t bs = cfg.block_size;
        const size_t oc = cfg.output_channels;
        const size_t ic = cfg.input_channels;

        output.assign(num_blocks * bs * oc, 0.0);

        std::vector<double> block_out(bs * oc, 0.0);
        std::vector<double> block_in(ic ? bs * ic : 0, 0.0);

        for (size_t b = 0; b < num_blocks; ++b) {
            std::fill(block_out.begin(), block_out.end(), 0.0);
            if (fn) {
                fn(block_out.data(), ic ? block_in.data() : nullptr,
                   static_cast<unsigned int>(bs));
            }
            std::copy(block_out.begin(), block_out.end(),
                      output.begin() + b * block_out.size());
        }
    }

    // RMS of one (de-interleaved) output channel over the captured buffer.
    double rms(size_t channel) const {
        const size_t oc = cfg.output_channels;
        if (channel >= oc || output.empty()) return 0.0;
        double sum = 0.0;
        size_t count = 0;
        for (size_t i = channel; i < output.size(); i += oc) {
            sum += output[i] * output[i];
            ++count;
        }
        return count ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
    }

    // Peak absolute value of one output channel.
    double peak(size_t channel) const {
        const size_t oc = cfg.output_channels;
        if (channel >= oc || output.empty()) return 0.0;
        double p = 0.0;
        for (size_t i = channel; i < output.size(); i += oc) {
            p = std::max(p, std::fabs(output[i]));
        }
        return p;
    }
};

} // namespace xrune
