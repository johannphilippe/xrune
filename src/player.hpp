 #pragma once
 #include <iostream>
 #include "sndfile/sndfile_node.h"
 #include "utilities.h"
 #include "tui.hpp"
 #include "xrune.h"

 #include <chrono>
 #include <thread>
 #include <iomanip>
 #include <sstream>
 #include <cmath>
 #include <vector>

 namespace xrune {

 struct player
 {
    player(xrune::options &options)
        : _snd_player(options.audio_file, options.loop, options.ksmps, options.samplerate)
        , _graph(xrune::create_graph(options.runtime, options.driver, 
            audio_context{_snd_player.n_inputs, _snd_player.n_outputs, _snd_player.bloc_size, options.samplerate}))
        , _exchange_buffers(_snd_player.n_outputs, _snd_player.bloc_size)
        , _audio_buffers(_snd_player.n_outputs, std::vector<double>(_snd_player.bloc_size) )
        , log_scale(options.log_scale)
    {

        //_graph.get()->list_devices();
        _graph.get()->add_node(&_snd_player);

        /*
            Communication buffer (spsc) with engine
        */
        _snd_player.post_process_callback = [this](node<double>* n)
        {
            for(size_t ch = 0; ch < n->n_outputs; ++ch)
            {
                size_t read = 0;
                while (read < n->bloc_size)
                {
                    size_t cnt = _exchange_buffers[ch].write(n->outputs[ch] + read, n->bloc_size - read);
                    read += cnt;
                    if (cnt == 0) // Buffer full for this channel, skip it
                        break;  // Break inner loop, continue to next channel
                }
            }
        };
    }

    void run()
    {
        tui::multi_channel_oscilloscope osc;
        osc.base_height = tui::get_height() - 4;
        osc.log_scale = log_scale;

        tui::console_input input;
         double duration = _snd_player.duration.load();
         _graph.get()->start_stream();
        const size_t term_height = tui::get_height();
        // Use a conservative oscilloscope height that fits in most terminals
        // Start with: available height minus 3 lines for status/progress
        const size_t available_height = (term_height > 5) ? (term_height - 5) : 1;
        // Force to odd for center row symmetry
        size_t printed_osc_height = (available_height % 2 == 0) ? (available_height - 1) : available_height;
        // Extra safety: if still too large, reduce
        while (printed_osc_height + 3 > term_height) {
            printed_osc_height -= 2;
        }
        const size_t total_height = printed_osc_height + 3;

        // Position cursor at a safe home before first frame
        std::cout << "\033[H";
        std::cout.flush();

        while (true)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); // Approx 30 FPS

            // Format stable position string as MM:SS.mmm 
            double posf = _snd_player.position.load();
            int pos_ms = static_cast<int>(std::floor(posf * 1000.0 + 0.5));
            int pos_s = pos_ms / 1000;
            int pos_m = pos_s / 60;
            int pos_sec = pos_s % 60;
            int pos_msec = pos_ms % 1000;

            int dur_ms = static_cast<int>(std::floor(duration * 1000.0 + 0.5));
            int dur_s = dur_ms / 1000;
            int dur_m = dur_s / 60;
            int dur_sec = dur_s % 60;

            std::ostringstream pos_ss;
            pos_ss << std::setfill('0') << pos_m << ":" << std::setw(2) << pos_sec << "." << std::setw(3) << pos_msec;
            std::ostringstream dur_ss;
            dur_ss << std::setfill('0') << dur_m << ":" << std::setw(2) << dur_sec;

            // Move to home and clear display
            std::cout << "\033[H\033[J";
            std::cout.flush();

            // Oscilloscope 
            osc.clear();
            osc.lines = printed_osc_height;
            
            size_t channels_with_data = 0;
            for(size_t ch = 0; ch < _snd_player.n_outputs; ++ch)
            {
            
            size_t read = _exchange_buffers[ch].read(_audio_buffers[ch].data(), _snd_player.bloc_size);
            if (read > 0) channels_with_data++;
            tui::color color = tui::sig_colors[ch % tui::sig_colors.size()];
            osc.set_channel(_audio_buffers[ch].data(), read, 
                tui::sig_chars[ch % tui::sig_chars.size()], color);

            }

            osc.render();

            // Blank line separator
            std::cout << "\n";
            
            // Playing info line
            tui::set_color(tui::color::bright_cyan);
            std::cout << "ᚷrune Playing... " << pos_ss.str() << " / " << dur_ss.str() 
                    << " [" << _snd_player.n_outputs << " ch, " << channels_with_data << " data]\n";
            
            // Progress bar line
            tui::set_color(tui::color::bright_yellow);
            tui::progress_bar(static_cast<float>(posf), static_cast<float>(duration));
            std::cout << "\n";
            std::cout.flush();

        input.enable_raw();

        std::optional<char> c = input.read_char();

        if (c.has_value()) {
            char ch = c.value();
            switch(ch)
            {
                case 'q': 
                    input.disable_raw();
                    _graph.get()->stop_stream();
                    
                    std::cout << "\033[H\033[J";
                    std::cout.flush();
                    return;
                case 'm':
                    osc.log_scale = !osc.log_scale;
                    break;
                case 'r':
                    _snd_player.seek(0);
                    break;
                default: 
                    break;
            }
        }
        input.disable_raw();
        }
    }

 protected:
    sndread_node<double> _snd_player;
    std::shared_ptr< graph<double> > _graph;
    std::vector< spsc<double> > _exchange_buffers;
    std::vector< std::vector<double> > _audio_buffers;
    bool log_scale;
 };

} // namespace xrune