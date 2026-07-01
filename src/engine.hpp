#pragma once
#include "core.hpp"
#include "instance.hpp"
#include "audio_backend.hpp"
#include "rtaudio_backend.hpp"
#include <readerwriterqueue.h>
#include <vector>
#include <algorithm>
#include <memory>

namespace xrune {

enum class command_type {
    register_instance,
    unregister_instance,
    set_parameter
};

struct command_event {
    command_type type;
    graph_instance* target = nullptr;
    size_t node_index = 0;       // which node within the instance
    size_t parameter_index = 0;  // which parameter of that node
    sample_t parameter_value = 0.0;
};

struct telemetry_event {
    graph_instance* completed = nullptr;
};

// The audio host + instance summing bus. It owns no topology; it runs whatever
// graph_instances are registered and sums their output terminals to the master.
// (Handle-based addressing, lifetimes, and cross-instance routing arrive in
// Phases 2-3; here commands still carry a raw instance pointer.)
struct engine {
    size_t sample_rate = 48000;
    size_t block_size = 128;
    size_t input_channels = 0;
    size_t output_channels = 2;

    std::vector<graph_instance*> active;
    std::vector<std::vector<sample_t>> master_buffers;

    moodycamel::ReaderWriterQueue<command_event> command_queue;
    moodycamel::ReaderWriterQueue<telemetry_event> telemetry_queue;

    std::unique_ptr<audio_backend> backend;

    engine() = default;
    ~engine() { stop(); }

    // Inject a backend (e.g. offline_backend) before init(); defaults to RtAudio.
    void use_backend(std::unique_ptr<audio_backend> b) { backend = std::move(b); }

    bool init(size_t sr = 48000, size_t bs = 128, size_t ins = 0, size_t outs = 2) {
        sample_rate = sr;
        block_size = bs;
        input_channels = ins;
        output_channels = outs;

        active.clear();
        active.reserve(128);

        master_buffers.resize(output_channels);
        for (auto& buf : master_buffers) buf.assign(block_size, 0.0);

        if (!backend) backend = std::make_unique<rtaudio_backend>();

        backend_config cfg{sample_rate, block_size, input_channels, output_channels};
        return backend->open(cfg, [this](double* out, const double* in, unsigned int n) {
            this->process(out, in, n);
        });
    }

    bool start() { return backend ? backend->start() : false; }

    void stop() {
        if (backend) {
            backend->stop();
            backend->close();
        }
    }

    void register_instance(graph_instance* g) {
        command_queue.enqueue({command_type::register_instance, g, 0, 0, 0.0});
    }

    void unregister_instance(graph_instance* g) {
        command_queue.enqueue({command_type::unregister_instance, g, 0, 0, 0.0});
    }

    void set_parameter(graph_instance* g, size_t node_index, size_t param, sample_t value) {
        command_queue.enqueue({command_type::set_parameter, g, node_index, param, value});
    }

    bool dequeue_completed(graph_instance*& g) {
        telemetry_event ev;
        if (telemetry_queue.try_dequeue(ev)) { g = ev.completed; return true; }
        return false;
    }

    int process(double* out_buf, const double* /*in_buf*/, unsigned int n_frames) {
        const size_t nf = std::min<size_t>(n_frames, block_size);

        // 1. Drain control commands.
        command_event cmd;
        while (command_queue.try_dequeue(cmd)) {
            switch (cmd.type) {
                case command_type::register_instance:
                    if (active.size() < active.capacity()) active.push_back(cmd.target);
                    break;
                case command_type::unregister_instance: {
                    auto it = std::find(active.begin(), active.end(), cmd.target);
                    if (it != active.end()) {
                        active.erase(it);
                        telemetry_queue.enqueue({cmd.target});
                    }
                    break;
                }
                case command_type::set_parameter:
                    if (cmd.target)
                        cmd.target->set_parameter(cmd.node_index, cmd.parameter_index,
                                                  cmd.parameter_value);
                    break;
            }
        }

        // 2. Clear master summing buffers.
        for (auto& buf : master_buffers) std::fill(buf.begin(), buf.end(), 0.0);

        // 3. Process instances and sum their output terminals.
        for (auto it = active.begin(); it != active.end(); ) {
            graph_instance* g = *it;
            if (g->finished_flag) {
                telemetry_queue.enqueue({g});
                it = active.erase(it);
                continue;
            }
            g->process();
            const size_t chans = std::min(g->output_channels(), output_channels);
            for (size_t ch = 0; ch < chans; ++ch) {
                audio_buffer_view v = g->output_view(ch);
                for (size_t i = 0; i < nf; ++i) master_buffers[ch][i] += v[i];
            }
            ++it;
        }

        // 4. Interleave to the device buffer.
        for (size_t i = 0; i < n_frames; ++i)
            for (size_t ch = 0; ch < output_channels; ++ch)
                out_buf[i * output_channels + ch] =
                    (i < nf) ? master_buffers[ch][i] : 0.0;

        return 0;
    }
};

} // namespace xrune
