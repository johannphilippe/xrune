#pragma once
#include <iostream>
#include <vector>
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

    void oscilloscope(const float *data, size_t size)
    {
        size_t width = get_width() - 2;
        float step = static_cast<float>(size) / static_cast<float>(width);
        std::cout << "|";
        for (size_t i = 0; i < width; ++i)
        {
            size_t index = static_cast<size_t>(i * step);
            float v = data[index];
            if (v > 0.5f)
                std::cout << "*";
            else if (v < -0.5f)
                std::cout << ".";
            else
                std::cout << " ";
        }
    }

    // Monophonic oscilloscope: time on X, amplitude on Y.
    // - data: pointer to `size` samples (mono)
    // - lines: number of rows to render (height)
    // This draws `lines` rows, top == +1.0, bottom == -1.0. Right side shows a few tick labels
    void multiline_oscilloscope(const double *data, size_t size, size_t lines, bool log_scale = true)
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
                double norm = (v + 1.0) / 2.0; // 0..1
                if (norm <= 0.0)
                    row = lines - 1;
                else if (norm >= 1.0)
                    row = 0;
                else
                    row = static_cast<size_t>((1.0 - norm) * (lines - 1) + 0.5);
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

                size_t center = (lines - 1) / 2;
                size_t max_off = center;
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
            plane[row][x] = '*';
        }

        // right-side tick labels (match chosen scale)
        std::vector<std::pair<size_t, std::string>> labels;
        if (!log_scale)
        {
            // linear labels: show amplitude values
            for (size_t t = 0; t < 5; ++t)
            {
                size_t r = static_cast<size_t>((static_cast<double>(t) / 4.0) * (lines - 1));
                double amp = 1.0 - (static_cast<double>(r) / (lines - 1)) * 2.0; // map back to [-1,1]
                std::ostringstream oss;
                oss.setf(std::ios::fixed);
                oss.precision(2);
                oss << (amp >= 0.0 ? ' ' : '-') << std::abs(amp);
                labels.emplace_back(r, oss.str());
            }
        }
        else
        {
            // log labels: produce symmetric labels around the center baseline.
            // Center shows `baseline_db` (e.g. -70 dB). Top and bottom show 0 dB.
            const double baseline_db = -70.0;
            size_t center = (lines - 1) / 2;
            size_t max_off = center;
            for (size_t t = 0; t < 5; ++t)
            {
                size_t r = static_cast<size_t>((static_cast<double>(t) / 4.0) * (lines - 1));
                size_t dist = (r > center) ? (r - center) : (center - r);
                double nd = (max_off == 0) ? 0.0 : (static_cast<double>(dist) / max_off);
                if (nd < 0.0) nd = 0.0;
                if (nd > 1.0) nd = 1.0;
                double db = baseline_db + nd * (0.0 - baseline_db); // baseline..0
                std::ostringstream oss;
                oss.setf(std::ios::fixed);
                oss.precision(0);
                oss << std::setw(4) << db << "dB";
                labels.emplace_back(r, oss.str());
            }
        }

        // print plane top-down, attaching labels to the right
        for (size_t r = 0; r < lines; ++r)
        {
            bool center_line = r == (lines - 1) / 2;
            if(center_line) {
                set_color(color::bright_yellow);
            } else {
                set_color(color::bright_cyan);
            }
            clear_line();
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
    
};
