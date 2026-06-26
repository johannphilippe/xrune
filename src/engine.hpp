#pragma once
#include "core.hpp"
#include "graph.hpp"
#include <RtAudio.h>
#include <readerwriterqueue.h>
#include <vector>
#include <iostream>
#include <algorithm>

namespace xrune {

enum class command_type {
    register_graph,
    unregister_graph,
    set_parameter
};

struct command_event {
    command_type type;
    graph* target_graph = nullptr;
    node* target_node = nullptr;
    size_t parameter_index = 0;
    sample_t parameter_value = 0.0;
};

struct telemetry_event {
    graph* completed_graph = nullptr;
};

struct engine {
    rt::audio::RtAudio dac;
    size_t sample_rate = 48000;
    size_t block_size = 128;
    size_t input_channels = 0;
    size_t output_channels = 2;

    std::vector<graph*> active_graphs;
    std::vector<std::vector<sample_t>> master_buffers;
    std::vector<std::vector<sample_t>> input_buffers;

    // Lock-free queues for control and telemetry
    moodycamel::ReaderWriterQueue<command_event> command_queue;
    moodycamel::ReaderWriterQueue<telemetry_event> telemetry_queue;

    engine() = default;
    
    ~engine() {
        stop();
    }

    bool init(size_t sr = 48000, size_t bs = 128, size_t ins = 0, size_t outs = 2) {
        sample_rate = sr;
        block_size = bs;
        input_channels = ins;
        output_channels = outs;

        // Pre-reserve active graphs to prevent allocations on the audio thread
        active_graphs.clear();
        active_graphs.reserve(128);

        master_buffers.resize(output_channels);
        for (auto& buf : master_buffers) {
            buf.assign(block_size, 0.0);
        }

        input_buffers.resize(input_channels);
        for (auto& buf : input_buffers) {
            buf.assign(block_size, 0.0);
        }

        rt::audio::RtAudio::StreamParameters out_params;
        out_params.deviceId = dac.getDefaultOutputDevice();
        out_params.nChannels = static_cast<unsigned int>(output_channels);
        out_params.firstChannel = 0;

        rt::audio::RtAudio::StreamParameters* in_params_ptr = nullptr;
        rt::audio::RtAudio::StreamParameters in_params;
        if (input_channels > 0) {
            in_params.deviceId = dac.getDefaultInputDevice();
            in_params.nChannels = static_cast<unsigned int>(input_channels);
            in_params.firstChannel = 0;
            in_params_ptr = &in_params;
        }

        unsigned int buffer_frames = static_cast<unsigned int>(block_size);
        
        rt::audio::RtAudio::StreamOptions options;
        rt::audio::RtAudioErrorType err = dac.openStream(
            &out_params, in_params_ptr, rt::audio::RTAUDIO_FLOAT64,
            static_cast<unsigned int>(sample_rate), &buffer_frames,
            &audio_callback, this, &options
        );

        if (err != rt::audio::RTAUDIO_NO_ERROR) {
            std::cerr << "RtAudio error on openStream: " << dac.getErrorText() << std::endl;
            return false;
        }

        return true;
    }

    bool start() {
        if (!dac.isStreamOpen()) return false;
        if (dac.isStreamRunning()) return true;
        
        rt::audio::RtAudioErrorType err = dac.startStream();
        if (err != rt::audio::RTAUDIO_NO_ERROR) {
            std::cerr << "RtAudio error on startStream: " << dac.getErrorText() << std::endl;
            return false;
        }
        return true;
    }

    void stop() {
        if (dac.isStreamRunning()) {
            dac.stopStream();
        }
        if (dac.isStreamOpen()) {
            dac.closeStream();
        }
    }

    // Thread-safe, lock-free registration
    void register_graph(graph* g) {
        command_event cmd;
        cmd.type = command_type::register_graph;
        cmd.target_graph = g;
        command_queue.enqueue(cmd);
    }

    // Thread-safe, lock-free unregistration
    void unregister_graph(graph* g) {
        command_event cmd;
        cmd.type = command_type::unregister_graph;
        cmd.target_graph = g;
        command_queue.enqueue(cmd);
    }

    // Thread-safe, lock-free parameter updates
    void set_parameter(node* n, size_t index, sample_t value) {
        command_event cmd;
        cmd.type = command_type::set_parameter;
        cmd.target_node = n;
        cmd.parameter_index = index;
        cmd.parameter_value = value;
        command_queue.enqueue(cmd);
    }

    // Poll completed graphs for deletion on the main thread
    bool dequeue_completed_graph(graph*& g) {
        telemetry_event ev;
        if (telemetry_queue.try_dequeue(ev)) {
            g = ev.completed_graph;
            return true;
        }
        return false;
    }

    // Process callback (100% real-time safe)
    int process(double* out_buf, const double* in_buf, unsigned int n_frames) {
        // 1. Process all pending control commands
        command_event cmd;
        while (command_queue.try_dequeue(cmd)) {
            if (cmd.type == command_type::register_graph) {
                if (active_graphs.size() < active_graphs.capacity()) {
                    active_graphs.push_back(cmd.target_graph);
                }
            } else if (cmd.type == command_type::unregister_graph) {
                auto it = std::find(active_graphs.begin(), active_graphs.end(), cmd.target_graph);
                if (it != active_graphs.end()) {
                    active_graphs.erase(it);
                    telemetry_queue.enqueue({cmd.target_graph});
                }
            } else if (cmd.type == command_type::set_parameter) {
                if (cmd.target_node) {
                    cmd.target_node->set_parameter(cmd.parameter_index, cmd.parameter_value);
                }
            }
        }

        // 2. Clear master output summing buffers
        for (auto& buf : master_buffers) {
            std::fill(buf.begin(), buf.end(), 0.0);
        }

        // 3. Deinterleave inputs
        if (in_buf && input_channels > 0) {
            for (size_t i = 0; i < n_frames; ++i) {
                for (size_t ch = 0; ch < input_channels; ++ch) {
                    input_buffers[ch][i] = in_buf[i * input_channels + ch];
                }
            }
        }

        // 4. Process all active graphs and sum outputs
        for (auto it = active_graphs.begin(); it != active_graphs.end(); ) {
            graph* g = *it;
            if (g->finished_flag) {
                telemetry_queue.enqueue({g});
                it = active_graphs.erase(it);
                continue;
            }

            if (g->input_node && input_channels > 0) {
                size_t in_idx = g->node_to_index[g->input_node];
                size_t chans = std::min(g->input_node->outputs_count(), input_channels);
                for (size_t ch = 0; ch < chans; ++ch) {
                    std::copy(input_buffers[ch].begin(), input_buffers[ch].end(),
                              g->output_buffers[in_idx][ch].begin());
                }
            }

            g->process_block(n_frames, sample_rate);

            if (g->output_node) {
                size_t out_idx = g->node_to_index[g->output_node];
                size_t chans = std::min(g->output_node->outputs_count(), output_channels);
                for (size_t ch = 0; ch < chans; ++ch) {
                    for (size_t i = 0; i < n_frames; ++i) {
                        master_buffers[ch][i] += g->output_buffers[out_idx][ch][i];
                    }
                }
            }
            ++it;
        }

        // 5. Interleave output buffer
        for (size_t i = 0; i < n_frames; ++i) {
            for (size_t ch = 0; ch < output_channels; ++ch) {
                out_buf[i * output_channels + ch] = master_buffers[ch][i];
            }
        }

        return 0;
    }

    private:
    static int audio_callback(void* output_buffer, void* input_buffer,
                              unsigned int n_frames, double stream_time,
                              rt::audio::RtAudioStreamStatus status, void* user_data) {
        auto* self = static_cast<engine*>(user_data);
        auto* out_buf = static_cast<double*>(output_buffer);
        const auto* in_buf = static_cast<const double*>(input_buffer);
        return self->process(out_buf, in_buf, n_frames);
    }
};

} // namespace xrune
