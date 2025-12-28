#pragma once
#include <iostream>
#include <array>

#include "combinator3000.h"

namespace xrune {
    enum class audio_runtime
    {
        rtaudio, 
        miniaudio, 
        csound,
    };

    constexpr std::array<const char *, 3> audio_runtime_str = 
    {
        "rtaudio", 
        "miniaudio", 
        "csound",
    };

    static audio_runtime parse_runtime(std::string &s)
    {
        for(size_t i = 0; i < audio_runtime_str.size(); ++i)
        {
            if(std::string(audio_runtime_str[i]) == s)
                return static_cast<audio_runtime>(i);
        }
        std::cout << "No matching runtime, backing up to RtAudio by default" << std::endl;
        return audio_runtime::rtaudio;
    }

    enum class audio_driver
    {
        // Linux 
        alsa = 0, 
        pulse = 1, 
        jack = 2, 
        unspecified = 3,
    };

    constexpr std::array<const char *, 4> audio_driver_str = 
    {
        "alsa", 
        "pulse", 
        "jack", 
        "unspecified"
    };

    static audio_driver parse_driver(std::string &s)
    {
        for(size_t i = 0; i < audio_driver_str.size(); ++i)
        {
            if(std::string(audio_driver_str[i]) == s)
            {
                return static_cast<audio_driver>(i);
            }

        }
        std::cout << "No matching runtime, backing up to RtAudio by default" << std::endl;
        return audio_driver::unspecified;
    }

    // CLI options
    struct options
    {
        bool exit_after_parse = false;
        bool loop = false;
        bool log_scale = false;
        bool play = false;
        std::string audio_file = "";
        size_t ksmps = 512;
        size_t samplerate = 48000;

        xrune::audio_runtime runtime = xrune::audio_runtime::rtaudio;
        xrune::audio_driver driver = xrune::audio_driver::unspecified;
    };

    static graph<double>* create_graph(xrune::audio_runtime rt, xrune::audio_driver driver, audio_context ctx)
    {
        switch(rt)
        {
            case(audio_runtime::rtaudio):
            {
                // TODO implement rtaudio missing drivers
                RtAudio::Api api = RtAudio::Api::UNSPECIFIED;
                switch(driver)
                {
                    case audio_driver::alsa:
                        api = RtAudio::Api::LINUX_ALSA;
                        break;
                    case audio_driver::jack:
                        api = RtAudio::Api::UNIX_JACK;
                        break;
                    case audio_driver::pulse:
                        api = RtAudio::Api::LINUX_PULSE;
                        break;
                    case audio_driver::unspecified:
                        api = RtAudio::Api::UNSPECIFIED;
                        break;
                    default:
                        api = RtAudio::Api::UNSPECIFIED;
                        break;
                }
                rtgraph<double> *g = new rtgraph<double> 
                    (ctx.n_inputs, ctx.n_outputs, ctx.bloc_size, ctx.samplerate, api);
                return static_cast< graph<double> *>(g);
            }
            case(audio_runtime::miniaudio):
            {
                // TODO implement miniaudio drivers selector
                mini_rtgraph<double> *g = new mini_rtgraph<double> 
                    (ctx.n_inputs, ctx.n_outputs, ctx.bloc_size, ctx.samplerate);
                return static_cast< graph<double> *>(g);

            }
            case(audio_runtime::csound):
            {
                #ifdef CSOUND_GRAPH
                    std::string audio_device;
                    switch(driver)
                    {
                        case audio_driver::alsa:
                            audio_device = "-+rtaudio=alsa";
                            break;
                        case audio_driver::jack:
                            audio_device = "-+rtaudio=jack";
                            break;
                        case audio_driver::pulse:
                            audio_device = "-+rtaudio=pulse";
                            break;
                        case audio_driver::unspecified:

                            break;
                        default:
                            break;
                    };
                    csound_rtgraph<double>* g = new csound_rtgraph<double>(
                        ctx.n_inputs, ctx.n_outputs, ctx.bloc_size, ctx.samplerate, audio_device);
                    // To implement 
                    return (graph<double> *)g;
                #else 
                    throw std::runtime_error("Xrune : Csound is not linked, not able to use it as runtime engine");
                #endif

            }
            default:
                return nullptr;
        }
    }
};