#pragma once
#include <functional>
#include <cstddef>

namespace xrune {

// Configuration handed to a backend when opening the stream.
struct backend_config {
    size_t sample_rate = 48000;
    size_t block_size = 128;
    size_t input_channels = 0;
    size_t output_channels = 2;
};

// The DSP callback a backend drives. Buffers are interleaved doubles.
// `in` may be nullptr when there are no input channels.
using audio_process_fn =
    std::function<void(double* out, const double* in, unsigned int n_frames)>;

// Thin abstraction over the audio host so the engine does not depend on a
// concrete device API. Implementations: rtaudio_backend (hardware),
// offline_backend (headless, for tests/CI).
struct audio_backend {
    virtual ~audio_backend() = default;

    // Acquire resources and register the DSP callback. Returns false on error.
    virtual bool open(const backend_config& cfg, audio_process_fn fn) = 0;

    // Begin / end calling the DSP callback.
    virtual bool start() = 0;
    virtual void stop() = 0;

    // Release resources.
    virtual void close() = 0;
};

} // namespace xrune
