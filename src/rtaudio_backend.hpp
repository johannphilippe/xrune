#pragma once
#include "audio_backend.hpp"
#include <RtAudio.h>
#include <iostream>

namespace xrune {

// Hardware audio backend wrapping RtAudio (the previous engine behaviour).
struct rtaudio_backend : audio_backend {
    rt::audio::RtAudio dac;
    backend_config cfg;
    audio_process_fn fn;

    ~rtaudio_backend() override {
        stop();
        close();
    }

    bool open(const backend_config& c, audio_process_fn f) override {
        cfg = c;
        fn = std::move(f);

        rt::audio::RtAudio::StreamParameters out_params;
        out_params.deviceId = dac.getDefaultOutputDevice();
        out_params.nChannels = static_cast<unsigned int>(cfg.output_channels);
        out_params.firstChannel = 0;

        rt::audio::RtAudio::StreamParameters in_params;
        rt::audio::RtAudio::StreamParameters* in_params_ptr = nullptr;
        if (cfg.input_channels > 0) {
            in_params.deviceId = dac.getDefaultInputDevice();
            in_params.nChannels = static_cast<unsigned int>(cfg.input_channels);
            in_params.firstChannel = 0;
            in_params_ptr = &in_params;
        }

        unsigned int buffer_frames = static_cast<unsigned int>(cfg.block_size);
        rt::audio::RtAudio::StreamOptions options;

        rt::audio::RtAudioErrorType err = dac.openStream(
            &out_params, in_params_ptr, rt::audio::RTAUDIO_FLOAT64,
            static_cast<unsigned int>(cfg.sample_rate), &buffer_frames,
            &rt_callback, this, &options);

        if (err != rt::audio::RTAUDIO_NO_ERROR) {
            std::cerr << "RtAudio openStream error: " << dac.getErrorText() << std::endl;
            return false;
        }
        return true;
    }

    bool start() override {
        if (!dac.isStreamOpen()) return false;
        if (dac.isStreamRunning()) return true;
        rt::audio::RtAudioErrorType err = dac.startStream();
        if (err != rt::audio::RTAUDIO_NO_ERROR) {
            std::cerr << "RtAudio startStream error: " << dac.getErrorText() << std::endl;
            return false;
        }
        return true;
    }

    void stop() override {
        if (dac.isStreamRunning()) dac.stopStream();
    }

    void close() override {
        if (dac.isStreamOpen()) dac.closeStream();
    }

private:
    static int rt_callback(void* output_buffer, void* input_buffer,
                           unsigned int n_frames, double /*stream_time*/,
                           rt::audio::RtAudioStreamStatus /*status*/, void* user_data) {
        auto* self = static_cast<rtaudio_backend*>(user_data);
        if (self->fn) {
            self->fn(static_cast<double*>(output_buffer),
                     static_cast<const double*>(input_buffer), n_frames);
        }
        return 0;
    }
};

} // namespace xrune
