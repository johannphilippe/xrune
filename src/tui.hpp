#pragma once
#include <iostream>
#include <vector>
#include <array>
#include <algorithm>
#include <string>
#include <sstream>
#include <utility>
#include <cmath>
#include <iomanip>

#ifdef _WIN32
    #include <windows.h>
    #include <conio.h>
#else
    #include <sys/ioctl.h>
    #include <unistd.h>
    #include <termios.h>
    #include <fcntl.h>
#endif

#include <optional>

namespace tui
{

    /*
        TUI input 
    */

       struct console_input {
        #ifdef _WIN32
            DWORD original_mode = 0;
        #else
            termios original_termios{};
        #endif

        void enable_raw() {
            #ifdef _WIN32
                HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
                GetConsoleMode(h, &original_mode);

                DWORD mode = original_mode;
                mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
                SetConsoleMode(h, mode);

            #else
                tcgetattr(STDIN_FILENO, &original_termios);

                termios raw = original_termios;
                raw.c_lflag &= ~(ICANON | ECHO);
                tcsetattr(STDIN_FILENO, TCSANOW, &raw);

                // make stdin non-blocking
                fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
            #endif
        }

        void disable_raw() {
            #ifdef _WIN32
                HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
                SetConsoleMode(h, original_mode);

            #else
                tcsetattr(STDIN_FILENO, TCSANOW, &original_termios);
            #endif
        }


        static std::optional<char> read_char() {
            #ifdef _WIN32
                if (_kbhit()) {
                    return static_cast<char>(_getch());
                }
                return std::nullopt;

            #else
                char c;
                ssize_t n = ::read(STDIN_FILENO, &c, 1);

                if (n > 0) {
                    return c;
                }
                return std::nullopt;
            #endif
        }
    };


    /*
        TUI output 
    */

    enum class color
    {
        reset = 0,
        black = 30,
        red = 31,
        green = 32,
        yellow = 33,
        blue = 34,
        magenta = 35,
        cyan = 36,
        white = 37,
        bright_black = 90,
        bright_red = 91,
        bright_green = 92,
        bright_yellow = 93,
        bright_blue = 94,
        bright_magenta = 95,
        bright_cyan = 96,
        bright_white = 97
    };

    constexpr std::array<color, 6> sig_colors = {
        color::bright_cyan,
        color::bright_yellow,
        color::bright_red,
        color::bright_green,
        color::bright_magenta,
        color::bright_blue,
    };

    constexpr std::array<char, 6> sig_chars = {
        'x', '*', '@', '#', '-', '&'
    };

    static void set_color(color c)
    {
        std::cout << "\033[" << static_cast<int>(c) << "m";
    }

    static void move_up(size_t lines = 1)
    {
        std::cout << "\033[" << lines << "A";
    }

    static void move_down(size_t lines = 1)
    {
        std::cout << "\033[" << lines << "B";
    }

    static void clear_line()
    {
        std::cout << "\033[2K\r";
    }

    static size_t get_width()
    {
#ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
#else
        struct winsize w;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        return w.ws_col;
#endif

    }

    static size_t get_height()
    {
#ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
        return csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
#else
        struct winsize w;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        return w.ws_row;
#endif

    }


    /*
    struct line 
    {
        line(size_t width) 
            : _data(width)
        {}
        std::vector<char> _data;
    };

    struct screen 
    {
        screen() 
            : _data(get_height(), get_width())
        {}

        void redraw() 
        {
            _data.resize(get_height());
            for(auto & it : _data)
                it._data.resize(get_width());
        }

        void print()


        std::vector<line> _data;
    };
    */


    void progress_bar(float progress, float max)
    {
        size_t width = get_width() - 10;
        float ratio = progress / max;
        size_t cwidth = static_cast<size_t>(ratio * width);
        std::cout << "[";
        for (size_t i = 0; i < width; ++i)
        {
            if (i < cwidth)
                std::cout << "=";
            else if (i == cwidth)
                std::cout << ">";
            else
                std::cout << " ";
        }
        std::cout << "] " << static_cast<size_t>(ratio * 100.0) << " %\r" << std::flush;
    }

    /*
    // Monophonic oscilloscope: time on X, amplitude on Y.
    // - data: pointer to `size` samples (mono)
    // - lines: number of rows to render (height)
    // This draws `lines` rows, top == +1.0, bottom == -1.0. Right side shows a few tick labels
    void multiline_oscilloscope(const double *data, size_t size, size_t lines,
        color c, char chr = 'x', bool log_scale = true)
    {
        if (!data || size == 0 || lines == 0)
            return;

        const size_t reserved_right = 8; // space for labels
        size_t width = 0;
        size_t term_w = get_width();
        if (term_w > reserved_right + 2)
            width = term_w - reserved_right - 2; // leave margins
        else
            width = 10;

        // prepare empty plane (lines rows, width columns)
        std::vector<std::string> plane(lines, std::string(width, ' '));

    // compute a consistent center index (use lround so even/odd heights
    // map consistently). We'll use this for mapping and labels so v==0
    // always maps to the same center row.
    size_t center_index = static_cast<size_t>(std::lround((lines - 1) / 2.0));

    // sample each column (time on X)
        for (size_t x = 0; x < width; ++x)
        {
            // map x in [0,width-1] to sample index in [0,size-1]
            size_t idx = static_cast<size_t>((static_cast<double>(x) / (width - 1)) * (size - 1));
            double v = data[idx];

            // Mapping: either linear (default) or log (dB-like) around center
            size_t row = 0;
            if (!log_scale)
            {
                // linear: assume data already scaled in [-1,1]
                // compute row with rounding so v==0 maps consistently to the
                // same center row we use for labels.
                double norm = (v + 1.0) / 2.0; // 0..1
                double pos = (1.0 - norm) * (lines - 1);
                row = static_cast<size_t>(std::lround(pos));
                if (row >= lines) row = lines - 1;
            }
            else
            {
                // log scale: map magnitude to dB, then map baseline_db to center
                // so small/silent values sit near the center and louder values
                // move away from center. This represents variations around a
                // chosen noise floor instead of mapping silence to the far
                // bottom/top.
                const double baseline_db = -70.0; // center line corresponds to -70 dB
                const double min_mag = std::pow(10.0, baseline_db / 20.0);
                double mag = std::max(std::abs(v), min_mag);
                double db = 20.0 * std::log10(mag); // <= 0

                // normalize db in [baseline_db .. 0] -> [0..1]
                double nd = (db - baseline_db) / (0.0 - baseline_db);
                if (nd < 0.0) nd = 0.0;
                if (nd > 1.0) nd = 1.0;

                size_t center = center_index;
                size_t max_off = center_index;
                size_t off = static_cast<size_t>(nd * max_off + 0.5);
                if (v >= 0.0)
                {
                    // positive above center
                    if (off > center) off = center;
                    row = (off > center) ? 0 : (center - off);
                }
                else
                {
                    // negative below center
                    if (off > max_off) off = max_off;
                    row = std::min(lines - 1, center + off);
                }
            }

            // mark amplitude point with '*'
            plane[row][x] = p;
        }

        // right-side tick labels (match chosen scale)
        std::vector<std::pair<size_t, std::string>> labels;
        if (!log_scale)
        {
            // linear labels: compute using the same center as the mapping so
            // the center label is exactly 0.0. Use a floating center to make
            // labels consistent for even/odd heights.
            double center_d = (lines - 1) / 2.0;
            for (size_t t = 0; t < 5; ++t)
            {
                size_t r = static_cast<size_t>(std::lround((static_cast<double>(t) / 4.0) * (lines - 1)));
                double amp = 0.0;
                // Use integer center_index so the center tick maps exactly to 0.00
                if (center_index != 0)
                {
                    amp = (static_cast<double>(center_index) - static_cast<double>(r)) / static_cast<double>(center_index); // range [-1..1]
                }
                std::ostringstream oss;
                oss.setf(std::ios::fixed);
                oss.precision(2);
                if (amp >= 0.0) oss << ' ';
                oss << std::setw(4) << amp;
                labels.emplace_back(r, oss.str());
            }
        }
        else
        {
            // log labels: produce symmetric labels around the center baseline.
            // Center shows `baseline_db` (e.g. -70 dB). Top and bottom show 0 dB.
            const double baseline_db = -70.0;
            size_t center = center_index;
            size_t max_off = center_index;
            for (size_t t = 0; t < 5; ++t)
            {
                size_t r = static_cast<size_t>(std::lround((static_cast<double>(t) / 4.0) * (lines - 1)));
                size_t dist = (r > center) ? (r - center) : (center - r);
                double nd = (max_off == 0) ? 0.0 : (static_cast<double>(dist) / max_off);
                if (nd < 0.0) nd = 0.0;
                if (nd > 1.0) nd = 1.0;
                double db = baseline_db + nd * (0.0 - baseline_db); // baseline..0
            if(center_line) {
                set_color(color::bright_yellow);
            } else {
                set_color(c);
            }
        }
        for (size_t r = 0; r < lines; ++r)
        {
            bool center_line = r == (lines) / 2;
            
            if(center_line) {
                set_color(color::bright_yellow);
            } else {
                set_color(c);
            }

            if(clear) clear_line();
            std::cout << "|";
            std::cout << plane[r];
            std::cout << "| ";
            // find label for this row (if any)
            bool printed = false;
            for (auto &p : labels)
            {
                if (p.first == r)
                {
                    std::cout << p.second;
                    printed = true;
                    break;
                }
            }
            if (!printed)
                std::cout << "     ";
            std::cout << "\n";
        }
        std::cout << std::flush;
    }
    */

    // Multi-channel oscilloscope: accumulate multiple channels before printing.
    // Each channel has a custom display character and color.
    // Time on X, amplitude on Y (same semantics as multiline_oscilloscope).
    struct multi_channel_oscilloscope
    {
        struct channel_config
        {
            const double *data = nullptr;
            size_t size = 0;
            char display_char = '*';
            color display_color = color::bright_cyan;
        };

        struct pixel
        {
            char ch = ' ';
            color col = color::bright_cyan;
        };

        std::vector<channel_config> channels;
        size_t lines = 16;
        bool log_scale = true;

        size_t base_height = 8;

        // Add a channel with custom display character and color
        void set_channel(const double *data, size_t size, char ch = '*', color c = color::bright_cyan)
        {
            channels.push_back({data, size, ch, c});
        }

        // Clear all accumulated channels
        void clear()
        {
            channels.clear();
        }

        // Print all accumulated channels to terminal
        void render()
        {
            if (channels.empty() || lines == 0)
                return;

            const size_t reserved_right = 8;
            size_t width = 0;
            size_t term_w = get_width();
            if (term_w > reserved_right + 2)
                width = term_w - reserved_right - 2;
            else
                width = 10;

            // Compute consistent center index
            size_t center_index = static_cast<size_t>(std::lround((lines - 1) / 2.0));

            // Prepare plane with pixel structs (char + color per position)
            // Use '\0' to indicate "no channel has drawn here yet" (so spaces don't overwrite)
            pixel empty_pixel{'\0', color::bright_cyan};
            std::vector<std::vector<pixel>> plane(lines, std::vector<pixel>(width, empty_pixel));

            // Render each channel
            for (const auto &ch : channels)
            {
                if (!ch.data || ch.size == 0)
                    continue;

                // Sample each column (time on X)
                for (size_t x = 0; x < width; ++x)
                {
                    // map x in [0,width-1] to sample index in [0,size-1]
                    size_t idx = static_cast<size_t>((static_cast<double>(x) / (width - 1)) * (ch.size - 1));
                    double v = ch.data[idx];

                    // Compute row (same logic as multiline_oscilloscope)
                    size_t row = 0;
                    if (!log_scale)
                    {
                        double norm = (v + 1.0) / 2.0;
                        double pos = (1.0 - norm) * (lines - 1);
                        row = static_cast<size_t>(std::lround(pos));
                        if (row >= lines) row = lines - 1;
                    }
                    else
                    {
                        const double baseline_db = -70.0;
                        const double min_mag = std::pow(10.0, baseline_db / 20.0);
                        double mag = std::max(std::abs(v), min_mag);
                        double db = 20.0 * std::log10(mag);
                        double nd = (db - baseline_db) / (0.0 - baseline_db);
                        if (nd < 0.0) nd = 0.0;
                        if (nd > 1.0) nd = 1.0;

                        // nd maps [0..1] to distance from center [0..center_index]
                        size_t dist = static_cast<size_t>(nd * center_index + 0.5);
                        if (dist > center_index) dist = center_index;
                        
                        // Map distance to row based on sign of v
                        if (v >= 0.0)
                        {
                            // Positive: above center
                            row = (dist > center_index) ? 0 : (center_index - dist);
                        }
                        else
                        {
                            // Negative: below center
                            row = std::min(lines - 1, center_index + dist);
                        }
                    }

                    // Mark this position with the channel's display character and color
                    plane[row][x] = {ch.display_char, ch.display_color};
                }
            }

            // Generate labels (linear or log)
            std::vector<std::pair<size_t, std::string>> labels;
            if (!log_scale)
            {
                double center_d = (lines - 1) / 2.0;
                for (size_t t = 0; t < 5; ++t)
                {
                    size_t r = static_cast<size_t>(std::lround((static_cast<double>(t) / 4.0) * (lines - 1)));
                    double amp = 0.0;
                    if (center_index != 0)
                    {
                        amp = (static_cast<double>(center_index) - static_cast<double>(r)) / static_cast<double>(center_index);
                    }
                    std::ostringstream oss;
                    oss.setf(std::ios::fixed);
                    oss.precision(2);
                    if (amp >= 0.0) oss << ' ';
                    oss << std::setw(4) << amp;
                    labels.emplace_back(r, oss.str());
                }
            }
            else
            {
                // log labels: symmetric around center baseline
                // For each label row, compute what dB value it corresponds to
                // by using the same row-to-dB logic as the rendering
                const double baseline_db = -70.0;
                size_t max_off = center_index;
                for (size_t t = 0; t < 5; ++t)
                {
                    size_t r = static_cast<size_t>(std::lround((static_cast<double>(t) / 4.0) * (lines - 1)));
                    
                    // Compute distance from center (mirroring the rendering logic)
                    size_t dist = (r > center_index) ? (r - center_index) : (center_index - r);
                    double nd = (max_off == 0) ? 0.0 : (static_cast<double>(dist) / max_off);
                    if (nd < 0.0) nd = 0.0;
                    if (nd > 1.0) nd = 1.0;
                    
                    // Map nd [0..1] to dB [baseline_db..0]
                    double db = baseline_db + nd * (0.0 - baseline_db);
                    
                    std::ostringstream oss;
                    oss.setf(std::ios::fixed);
                    oss.precision(0);
                    oss << std::setw(4) << db << "dB";
                    labels.emplace_back(r, oss.str());
                }
            }

            // Compute center row index
            size_t center_row = center_index;
            
            // Print plane top-down with per-character colors
            for (size_t r = 0; r < lines; ++r)
            {
                clear_line();
                
                // Left border in white
                set_color(color::bright_white);
                std::cout << "|";
                set_color(color::reset);
                
                // Print each character with its own color, skip empty positions
                for (size_t x = 0; x < width; ++x)
                {
                    if (plane[r][x].ch != '\0')
                    {
                        set_color(plane[r][x].col);
                        std::cout << plane[r][x].ch;
                        set_color(color::reset);
                    }
                    else
                    {
                        // Center line shows '-' in white, other rows show space
                        if (r == center_row)
                        {
                            set_color(color::bright_white);
                            std::cout << '-';
                            set_color(color::reset);
                        }
                        else
                        {
                            std::cout << ' ';
                        }
                    }
                }
                
                // Right border in white
                set_color(color::bright_white);
                std::cout << "|";
                set_color(color::reset);
                std::cout << " ";

                // Print labels
                bool printed = false;
                for (const auto &p : labels)
                {
                    if (p.first == r)
                    {
                        std::cout << p.second;
                        printed = true;
                        break;
                    }
                }
                if (!printed)
                    std::cout << "     ";
                std::cout << "\n";
            }
            std::cout << std::flush;
        }
    };
    
};
