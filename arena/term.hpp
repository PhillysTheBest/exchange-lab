// arena/term.hpp — the four things the viewer needs from a terminal.
//
// Enable 24-bit colour, ask how big the window is, read a key without blocking,
// and put it all back on the way out. No curses, no termcap, no third-party
// library: two small platform blocks, matching how src/platform_win32.cpp and
// src/platform_linux.cpp split the engine's OS needs.
//
// This header reaches for <windows.h> and <termios.h>, so exactly one
// translation unit includes it: arena/arena.cpp. Nothing in the session, the
// sweep or the tests touches it, which is why the viewer can be left out of a
// build entirely without taking the experiment with it.
//
// None of this is on any measured path. The engine's benchmarks do not link it.
#pragma once

#include <cstdio>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace px::arena {

struct TermSize {
    int cols = 80;
    int rows = 24;
};

// Puts the terminal into the state the renderer needs and restores it in the
// destructor, so a thrown exception or an early return cannot leave a user with
// no cursor and no echo.
class Terminal {
public:
    Terminal() {
#ifdef _WIN32
        out_ = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out_ != INVALID_HANDLE_VALUE && GetConsoleMode(out_, &saved_out_mode_)) {
            saved_out_ok_ = true;
            // Without this, 24-bit colour escapes are printed literally on
            // older consoles. If it fails, say so rather than drawing garbage.
            vt_ok_ = SetConsoleMode(out_, saved_out_mode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
        }
#else
        if (tcgetattr(STDIN_FILENO, &saved_) == 0) {
            saved_ok_ = true;
            termios raw = saved_;
            raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;  // a read returns immediately
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        vt_ok_ = true;
#endif
        std::fputs("\x1b[?25l", stdout); // hide the cursor
        std::fflush(stdout);
    }

    ~Terminal() {
        std::fputs("\x1b[0m\x1b[?25h\n", stdout); // reset colour, show the cursor
        std::fflush(stdout);
#ifdef _WIN32
        if (saved_out_ok_) SetConsoleMode(out_, saved_out_mode_);
#else
        if (saved_ok_) tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
#endif
    }

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    // True when 24-bit colour escapes will be interpreted rather than printed.
    [[nodiscard]] bool colour_ok() const noexcept { return vt_ok_; }

    [[nodiscard]] static TermSize size() noexcept {
        TermSize s;
#ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
            s.cols = info.srWindow.Right - info.srWindow.Left + 1;
            s.rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        }
#else
        winsize w{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
            s.cols = w.ws_col;
            s.rows = w.ws_row;
        }
#endif
        if (s.cols < 20) s.cols = 20;
        if (s.rows < 10) s.rows = 10;
        return s;
    }

    // 0 when nothing was typed. Never blocks, so the frame rate is the viewer's
    // own business and a session is never waiting on a human.
    [[nodiscard]] static int poll_key() noexcept {
#ifdef _WIN32
        if (_kbhit() == 0) return 0;
        const int c = _getch();
        if (c == 0 || c == 0xE0) {
            _getch(); // a function or arrow key: swallow the second byte
            return 0;
        }
        return c;
#else
        unsigned char c = 0;
        return read(STDIN_FILENO, &c, 1) == 1 ? static_cast<int>(c) : 0;
#endif
    }

private:
    bool vt_ok_ = false;
#ifdef _WIN32
    HANDLE out_ = nullptr;
    DWORD saved_out_mode_ = 0;
    bool saved_out_ok_ = false;
#else
    termios saved_{};
    bool saved_ok_ = false;
#endif
};

} // namespace px::arena
