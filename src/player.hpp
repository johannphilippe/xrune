 #pragma once
 #include <iostream>
 #include "sndfile/sndfile_node.h"
 #include "utilities.h"
 #include "tui.hpp"

 // To remove later
 #include <chrono>
 #include <thread>
 #include <iomanip>
 #include <sstream>
 #include <cmath>

 namespace xrune {

 struct player
 {
     player(std::string filename, bool loop = false, bool log_sc = true, 
        size_t ksmps = 128, size_t samplerate = 48000)
         : _snd_player(filename, loop, ksmps, samplerate)
         , _graph(0, _snd_player.n_outputs, _snd_player.bloc_size, _snd_player.sample_rate)
         , _exchange_buffer(_snd_player.bloc_size)
         , _audio_buffer(new double[_snd_player.bloc_size], std::default_delete<double[]>())
         , log_scale(log_sc)
     {
         _graph.add_node(&_snd_player);
         _snd_player.post_process_callback = [this](node<double>* n)
         {
             size_t read = 0;
             while (read < n->bloc_size)
             {
                 size_t cnt = _exchange_buffer.write(n->outputs[0] + read, n->bloc_size - read);
                 read += cnt;
                 if (cnt == 0) // Buffer full, wait a bit
                     return;
             }
         };
     }

     void run()
     {
        tui::console_input input;
         double duration = _snd_player.duration.load();
         _graph.start_stream();
        const size_t term_height = tui::get_height();
        const size_t osc_height = term_height - 4; 
        const size_t total_height = osc_height + 2; 

         while (true)
         {
             // Read from exchange buffer (mono)
             size_t read = _exchange_buffer.read(_audio_buffer.get(), _snd_player.bloc_size);

             std::this_thread::sleep_for(std::chrono::milliseconds(33)); // Approx 30 FPS

             // Format stable position string as MM:SS.mmm to avoid 1-char jitter
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

             tui::clear_line();
             std::cout << "\n";
             tui::multiline_oscilloscope(_audio_buffer.get(), read, osc_height, log_scale);
             tui::clear_line();
             tui::set_color(tui::color::bright_cyan);
             std::cout << "\rᚷrune Playing... " << pos_ss.str() << " / " << dur_ss.str() << "\n";
             tui::clear_line();
             tui::set_color(tui::color::bright_yellow);
             tui::progress_bar(static_cast<float>(posf), static_cast<float>(duration));
             std::cout << "\n";

            input.enable_raw();
            std::optional<char> c = tui::console_input::read_char();
            if (c.has_value()) {
                char ch = c.value();
                switch(ch)
                {
                    case 'q': 
                        input.disable_raw();
                        _graph.stop_stream();
                        
                        for(size_t i = 0; i < total_height; ++i)
                        {
                            tui::move_up(1);
                            tui::clear_line();
                        }

                        std::cout << "\nExiting player.\n";
                        return;
                    case 'm':
                        log_scale = !log_scale;
                        break;
                    case 'r':
                        _snd_player.seek(0);
                        break;
                    default: 
                        break;
                }
            }
            input.disable_raw();



             tui::move_up(3 + osc_height);
         }
     }

 protected:
     sndread_node<double> _snd_player;
     rtgraph<double> _graph;
     spsc<double> _exchange_buffer;
     std::shared_ptr<double> _audio_buffer;
     bool log_scale;
 };

} // namespace xrune