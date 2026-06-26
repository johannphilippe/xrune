#pragma once
#include <cstddef>
#include <vector>

namespace xrune {

using sample_t = double;

struct audio_buffer_view {
    sample_t* data = nullptr;
    size_t size = 0;

    audio_buffer_view() = default;
    audio_buffer_view(sample_t* d, size_t s) : data(d), size(s) {}

    sample_t& operator[](size_t idx) const { return data[idx]; }
};

struct node_processing_context {
    std::vector<audio_buffer_view> inputs;
    std::vector<audio_buffer_view> outputs;
    size_t sample_rate = 48000;
    size_t block_size = 128;
};

struct node {
    virtual ~node() = default;
    virtual size_t inputs_count() const = 0;
    virtual size_t outputs_count() const = 0;
    virtual void process(const node_processing_context& ctx) = 0;
    virtual void set_parameter(size_t index, sample_t value) {}
};

} // namespace xrune
