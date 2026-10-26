// app/line.hpp — the prompt's line editor: history, cursor keys, tab completion.
//
// Its own raw mode, rather than arena/term.hpp's: the viewer wants reads that
// never block, the prompt wants reads that always do, and term.hpp stays
// included by exactly one translation unit. The terminal is put back the way it
// was before every line is returned, so a child started for that line finds
// the terminal as a shell would leave it.
//
// Keys: left/right, home/end (also Ctrl-A/Ctrl-E), up/down through history,
// backspace and delete, Ctrl-U and Ctrl-K to clear before or after the cursor,
// Ctrl-C to drop the line, Ctrl-D on an empty line to leave, tab to complete.
// It counts one byte as one column, which is right for everything the app
// itself completes; a line longer than the terminal is wide redraws untidily.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <conio.h>
#else
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace px::app {

class LineEditor {
public:
    // Given the line up to the cursor, set word_start to where the word being
    // completed begins and return every word it could become.
    using Completer = std::function<std::vector<std::string>(const std::string& line, std::size_t& word_start)>;

    explicit LineEditor(Completer completer) : completer_(std::move(completer)) {}

    // False when input has ended (Ctrl-D on an empty line, or a closed stdin).
    [[nodiscard]] bool read(const std::string& prompt, std::string& out) {
        RawMode raw;
        if (!raw.ok()) return false;
        buf_.clear();
        pos_ = 0;
        hist_ = history_.size();
        prompt_ = prompt;
        redraw();
        for (;;) {
            int key = next_key();
            switch (key) {
            case kEof:
                std::fputs("\n", stdout);
                return false;
            case '\r':
            case '\n':
                std::fputs("\n", stdout);
                out = buf_;
                if (!buf_.empty() && (history_.empty() || history_.back() != buf_)) history_.push_back(buf_);
                return true;
            case 3: // Ctrl-C
                std::fputs("^C\n", stdout);
                buf_.clear();
                pos_ = 0;
                hist_ = history_.size();
                break;
            case 4: // Ctrl-D
                if (buf_.empty()) {
                    std::fputs("\n", stdout);
                    return false;
                }
                if (pos_ < buf_.size()) buf_.erase(pos_, 1);
                break;
            case kDelete:
                if (pos_ < buf_.size()) buf_.erase(pos_, 1);
                break;
            case 127:
            case 8:
                if (pos_ > 0) buf_.erase(--pos_, 1);
                break;
            case 1:
            case kHome:
                pos_ = 0;
                break;
            case 5:
            case kEnd:
                pos_ = buf_.size();
                break;
            case 2:
            case kLeft:
                if (pos_ > 0) --pos_;
                break;
            case 6:
            case kRight:
                if (pos_ < buf_.size()) ++pos_;
                break;
            case 21: // Ctrl-U
                buf_.erase(0, pos_);
                pos_ = 0;
                break;
            case 11: // Ctrl-K
                buf_.erase(pos_);
                break;
            case 16:
            case kUp:
                if (hist_ > 0) {
                    buf_ = history_[--hist_];
                    pos_ = buf_.size();
                }
                break;
            case 14:
            case kDown:
                if (hist_ < history_.size()) {
                    ++hist_;
                    buf_ = hist_ == history_.size() ? std::string() : history_[hist_];
                    pos_ = buf_.size();
                }
                break;
            case '\t':
                complete();
                break;
            default:
                if (key >= 32 && key < 256 && key != 127) buf_.insert(pos_++, 1, static_cast<char>(key));
                break;
            }
            redraw();
        }
    }

private:
    enum : int { kEof = -1, kNone = 256, kUp, kDown, kLeft, kRight, kHome, kEnd, kDelete };

    void redraw() const {
        std::string s = "\r" + prompt_ + buf_ + "\x1b[K";
        if (pos_ < buf_.size()) s += "\x1b[" + std::to_string(buf_.size() - pos_) + "D";
        std::fputs(s.c_str(), stdout);
        std::fflush(stdout);
    }

    void complete() {
        if (!completer_) return;
        const std::string before = buf_.substr(0, pos_);
        std::size_t start = pos_;
        std::vector<std::string> options = completer_(before, start);
        if (options.empty() || start > pos_) return;
        const std::string word = before.substr(start);
        std::string common = options[0];
        for (const std::string& o : options) {
            std::size_t n = 0;
            while (n < common.size() && n < o.size() && common[n] == o[n]) ++n;
            common.resize(n);
        }
        if (options.size() == 1 && !common.empty() && common.back() != '/' && common.back() != ',') common += ' ';
        if (common.size() > word.size()) {
            buf_.replace(start, pos_ - start, common);
            pos_ = start + common.size();
            return;
        }
        if (options.size() > 1) {
            std::sort(options.begin(), options.end());
            std::string list = "\n";
            for (const std::string& o : options) list += o + "  ";
            std::fputs((list + "\n").c_str(), stdout);
        }
    }

#ifdef _WIN32
    struct RawMode {
        [[nodiscard]] bool ok() const { return true; }
    };

    static int next_key() {
        const int c = _getch();
        if (c == 0 || c == 224) {
            switch (_getch()) {
            case 72: return kUp;
            case 80: return kDown;
            case 75: return kLeft;
            case 77: return kRight;
            case 71: return kHome;
            case 79: return kEnd;
            case 83: return kDelete;
            default: return kNone;
            }
        }
        if (c == 26) return 4; // Ctrl-Z, the Windows end of input
        return c;
    }
#else
    struct RawMode {
        termios saved{};
        bool good = false;
        RawMode() {
            if (tcgetattr(STDIN_FILENO, &saved) != 0) return;
            termios raw = saved;
            raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
            raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            good = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
        }
        ~RawMode() {
            if (good) tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        }
        RawMode(const RawMode&) = delete;
        RawMode& operator=(const RawMode&) = delete;
        [[nodiscard]] bool ok() const { return good; }
    };

    static int byte(int wait_ms) {
        if (wait_ms >= 0) {
            pollfd p{STDIN_FILENO, POLLIN, 0};
            if (poll(&p, 1, wait_ms) <= 0) return kNone;
        }
        unsigned char c = 0;
        return ::read(STDIN_FILENO, &c, 1) == 1 ? static_cast<int>(c) : static_cast<int>(kEof);
    }

    // An escape sequence arrives in one burst; an Esc pressed on its own does
    // not, so a short wait tells them apart.
    static int next_key() {
        const int c = byte(-1);
        if (c != 27) return c;
        const int b = byte(50);
        if (b != '[' && b != 'O') return kNone;
        int d = byte(50);
        std::string digits;
        while (d >= '0' && d <= '9') {
            digits += static_cast<char>(d);
            d = byte(50);
        }
        switch (d) {
        case 'A': return kUp;
        case 'B': return kDown;
        case 'C': return kRight;
        case 'D': return kLeft;
        case 'H': return kHome;
        case 'F': return kEnd;
        case '~':
            if (digits == "1" || digits == "7") return kHome;
            if (digits == "4" || digits == "8") return kEnd;
            if (digits == "3") return kDelete;
            return kNone;
        default: return kNone;
        }
    }
#endif

    Completer completer_;
    std::vector<std::string> history_;
    std::size_t hist_ = 0;
    std::string buf_;
    std::size_t pos_ = 0;
    std::string prompt_;
};

} // namespace px::app
