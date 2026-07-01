#pragma once
#include "core.hpp"
#include "instance.hpp"
#include "instance_manager.hpp"
#include "audio_backend.hpp"
#include "rtaudio_backend.hpp"
#include <readerwriterqueue.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <memory>

namespace xrune {

enum class command_type {
    activate,       // a control-built instance is ready to run
    kill,           // stop and reclaim an instance
    set_parameter
};

struct command_event {
    command_type type;
    instance_handle handle;
    size_t node_index = 0;
    size_t parameter_index = 0;
    sample_t parameter_value = 0.0;
};

// Audio -> control: a slot has been released and can be recycled.
struct telemetry_event {
    uint32_t slot = 0;
};

// Audio host + instance summing bus with a handle-addressed instance manager.
//
// Threading model (no locks, no audio-thread alloc/free):
//  - CONTROL thread: spawn() builds the instance (heap alloc allowed) then
//    enqueues `activate`; kill()/set_parameter() enqueue commands; reclaim()
//    drains released slots and frees them.
//  - AUDIO thread (process): drains commands, runs active instances, sums them,
//    and auto-reaps per lifetime policy. Validity is checked against
//    `active_gen` (audio-owned), so stale handles are dropped safely.
struct engine {
    size_t sample_rate = 48000;
    size_t block_size = 128;
    size_t input_channels = 0;
    size_t output_channels = 2;

    instance_manager mgr;

    // Audio-thread-owned scheduling state.
    std::vector<uint32_t> active_gen;   // per slot: generation currently active, 0 = inactive
    std::vector<uint32_t> active_list;  // slot indices currently processing

    std::vector<std::vector<sample_t>> master_buffers;

    moodycamel::ReaderWriterQueue<command_event> command_queue;
    moodycamel::ReaderWriterQueue<telemetry_event> telemetry_queue;

    std::unique_ptr<audio_backend> backend;

    engine() = default;
    ~engine() { stop(); }

    void use_backend(std::unique_ptr<audio_backend> b) { backend = std::move(b); }

    bool init(size_t sr = 48000, size_t bs = 128, size_t ins = 0, size_t outs = 2,
              size_t max_instances = 128) {
        sample_rate = sr;
        block_size = bs;
        input_channels = ins;
        output_channels = outs;

        mgr.init(max_instances);
        active_gen.assign(max_instances, 0);
        active_list.clear();
        active_list.reserve(max_instances);

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

    // ---- Control-thread API ----

    // Build an instance from a compiled schedule and schedule it to run.
    instance_handle spawn(const compiled_schedule& sched, lifetime_policy life = {}) {
        reclaim(); // return any released slots to the pool first
        instance_handle h = mgr.create(sched, sample_rate, life);
        if (!h.valid()) return null_handle;
        command_queue.enqueue({command_type::activate, h, 0, 0, 0.0});
        return h;
    }

    void kill(instance_handle h) {
        command_queue.enqueue({command_type::kill, h, 0, 0, 0.0});
    }

    void set_parameter(instance_handle h, size_t node_index, size_t param, sample_t value) {
        command_queue.enqueue({command_type::set_parameter, h, node_index, param, value});
    }

    // Drain released slots and free them. Returns how many were reclaimed.
    size_t reclaim() {
        size_t n = 0;
        telemetry_event ev;
        while (telemetry_queue.try_dequeue(ev)) {
            mgr.recycle(ev.slot);
            ++n;
        }
        return n;
    }

    bool is_valid(instance_handle h) const { return mgr.is_valid(h); }

    // Number of instances currently processing (audio-thread state; stable to
    // read after process()/render() when the audio thread is quiescent).
    size_t active_count() const { return active_list.size(); }

    // ---- Audio thread ----

    int process(double* out_buf, const double* /*in_buf*/, unsigned int n_frames) {
        const size_t nf = std::min<size_t>(n_frames, block_size);

        // 1. Drain control commands.
        command_event cmd;
        while (command_queue.try_dequeue(cmd)) {
            switch (cmd.type) {
                case command_type::activate: {
                    const uint32_t slot = cmd.handle.slot;
                    if (slot < active_gen.size() && active_gen[slot] == 0) {
                        active_gen[slot] = cmd.handle.generation;
                        active_list.push_back(slot);
                    }
                    break;
                }
                case command_type::kill: {
                    const uint32_t slot = cmd.handle.slot;
                    if (slot < active_gen.size() && active_gen[slot] == cmd.handle.generation) {
                        release_slot(slot);
                    }
                    break;
                }
                case command_type::set_parameter: {
                    const uint32_t slot = cmd.handle.slot;
                    if (slot < active_gen.size() && active_gen[slot] == cmd.handle.generation) {
                        if (auto* g = mgr.instance_at(slot))
                            g->set_parameter(cmd.node_index, cmd.parameter_index,
                                             cmd.parameter_value);
                    }
                    break;
                }
            }
        }

        // 2. Clear master summing buffers.
        for (auto& buf : master_buffers) std::fill(buf.begin(), buf.end(), 0.0);

        // 3. Process active instances, sum, and auto-reap. Compact active_list.
        size_t w = 0;
        for (size_t r = 0; r < active_list.size(); ++r) {
            const uint32_t slot = active_list[r];
            instance_slot& s = mgr.slots[slot];
            graph_instance* g = s.inst.get();

            g->process();

            const size_t chans = std::min(g->output_channels(), output_channels);
            sample_t block_peak = 0.0;
            for (size_t ch = 0; ch < chans; ++ch) {
                audio_buffer_view v = g->output_view(ch);
                for (size_t i = 0; i < nf; ++i) {
                    master_buffers[ch][i] += v[i];
                    block_peak = std::max(block_peak, std::fabs(v[i]));
                }
            }

            if (should_reap(s, g, block_peak)) {
                active_gen[slot] = 0;
                telemetry_queue.enqueue({slot});
                // dropped from active_list by not copying it forward
            } else {
                active_list[w++] = slot;
            }
        }
        active_list.resize(w);

        // 4. Interleave to the device buffer.
        for (size_t i = 0; i < n_frames; ++i)
            for (size_t ch = 0; ch < output_channels; ++ch)
                out_buf[i * output_channels + ch] = (i < nf) ? master_buffers[ch][i] : 0.0;

        return 0;
    }

private:
    void release_slot(uint32_t slot) {
        active_gen[slot] = 0;
        auto it = std::find(active_list.begin(), active_list.end(), slot);
        if (it != active_list.end()) active_list.erase(it);
        telemetry_queue.enqueue({slot});
    }

    static bool should_reap(instance_slot& s, graph_instance* g, sample_t block_peak) {
        if (g->finished_flag) return true;
        s.age_blocks++;
        switch (s.life.kind) {
            case lifetime_kind::permanent:
                return false;
            case lifetime_kind::timed:
                return s.age_blocks >= s.life.ttl_blocks;
            case lifetime_kind::until_finished:
                return false; // handled by finished_flag above
            case lifetime_kind::until_silent:
                if (block_peak < s.life.silence_threshold) s.silent_run++;
                else s.silent_run = 0;
                return s.silent_run >= s.life.silence_blocks;
        }
        return false;
    }
};

} // namespace xrune
