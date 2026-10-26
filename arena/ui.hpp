// arena/ui.hpp — a retro desktop drawn in 24-bit ANSI, and nothing else.
//
// It owns a grid of coloured cells and knows how to draw chunky windows with
// hard drop shadows, block-character meters and modal dialogs. It knows nothing
// about trading: no price, no order, no participant appears anywhere in this
// file. arena/arena.cpp is the only place the two meet, which is what keeps the
// renderer from growing into the project.
//
// HOW IT DRAWS. Every frame composes into the cell grid, then present() emits
// one string and writes it once. Colour escapes are only emitted where a colour
// actually changes, which turns a full 120x40 repaint into a few kilobytes
// rather than 40,000 escape sequences, and a single write means a frame can
// never be seen half-drawn. The cursor is parked at home rather than the screen
// being cleared, so there is no flash between frames.
//
// WHY NO CURSES. The engine has no third-party dependencies and this does not
// change that. What is actually needed here is a 2D array of coloured glyphs and
// one write syscall; ncurses would bring a build dependency, a wide-character
// configuration problem and an input model, in exchange for optimising a repaint
// that is already small enough to not matter at thirty frames a second.
//
// Glyphs are UTF-8 strings of up to four bytes, so block elements and box
// drawing work without a wide-char build. Width is assumed to be one column per
// glyph: every character used here is single-width, and the drawing helpers are
// the only place glyphs are chosen.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <string>
#include <vector>

namespace px::arena::ui {

struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
    [[nodiscard]] friend bool operator==(Rgb a, Rgb b) noexcept {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    }
};

// The palette. A deep purple desktop with hot title bars: magenta, teal, orange
// and violet, each window a different one so a glance finds the right pane.
namespace pal {
inline constexpr Rgb desktop{0x2a, 0x1b, 0x4a};
inline constexpr Rgb grid{0x3b, 0x27, 0x66};
inline constexpr Rgb shadow{0x12, 0x0b, 0x24};
inline constexpr Rgb panel{0x1a, 0x12, 0x33};
inline constexpr Rgb panel_edge{0x4a, 0x33, 0x7a};
inline constexpr Rgb ink{0xe8, 0xdf, 0xff};
inline constexpr Rgb dim{0x8f, 0x7f, 0xbf};
inline constexpr Rgb magenta{0xff, 0x2d, 0x95};
inline constexpr Rgb teal{0x00, 0xd4, 0xc0};
inline constexpr Rgb orange{0xff, 0x8c, 0x32};
inline constexpr Rgb violet{0x9d, 0x4e, 0xdd};
inline constexpr Rgb bar_bg{0x2e, 0x20, 0x52};
inline constexpr Rgb up{0x3d, 0xe0, 0x7a};
inline constexpr Rgb down{0xff, 0x4d, 0x5e};
inline constexpr Rgb title_ink{0x16, 0x08, 0x22};
} // namespace pal

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] int right() const noexcept { return x + w; }
    [[nodiscard]] int bottom() const noexcept { return y + h; }
};

class Screen {
public:
    Screen(int w, int h) { resize(w, h); }

    void resize(int w, int h) {
        w_ = w < 1 ? 1 : w;
        h_ = h < 1 ? 1 : h;
        cells_.assign(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), Cell{});
        out_.reserve(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_) * 8 + 64);
    }

    [[nodiscard]] int width() const noexcept { return w_; }
    [[nodiscard]] int height() const noexcept { return h_; }

    void clear(Rgb bg) {
        for (Cell& c : cells_) c = Cell{{' ', 0, 0, 0}, 1, bg, bg};
    }

    void set(int x, int y, const char* glyph, Rgb fg, Rgb bg) {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
        Cell& c = cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)];
        std::size_t n = glyph == nullptr ? 0 : std::strlen(glyph);
        if (n > sizeof c.g) n = sizeof c.g;
        if (n == 0) {
            c.g[0] = ' ';
            c.len = 1;
        } else {
            std::memcpy(c.g, glyph, n);
            c.len = static_cast<std::uint8_t>(n);
        }
        c.fg = fg;
        c.bg = bg;
    }

    void fill(const Rect& r, Rgb bg, const char* glyph = " ") {
        for (int y = r.y; y < r.bottom(); ++y) {
            for (int x = r.x; x < r.right(); ++x) set(x, y, glyph, bg, bg);
        }
    }

    // Writes text, clipped to `limit` columns and to the screen, and returns how
    // many columns it used so callers can lay fields out left to right.
    //
    // UTF-8 aware, one cell per code point. It has to be: the first version
    // advanced one byte at a time, which quietly cut every block element and
    // arrow into two or three cells of broken bytes. The ASCII columns looked
    // right and every glyph came out as a replacement character. All the glyphs
    // used here are single-width, so one code point is one column.
    int text(int x, int y, const char* s, Rgb fg, Rgb bg, int limit = -1) {
        int drawn = 0;
        for (const char* p = s; *p != '\0';) {
            if (limit >= 0 && drawn >= limit) break;
            const int n = utf8_len(static_cast<unsigned char>(*p));
            char glyph[5] = {};
            int got = 0;
            for (; got < n && p[got] != '\0'; ++got) glyph[got] = p[got];
            set(x + drawn, y, glyph, fg, bg);
            ++drawn;
            p += got == 0 ? 1 : got;
        }
        return drawn;
    }

    int text(int x, int y, const std::string& s, Rgb fg, Rgb bg, int limit = -1) {
        return text(x, y, s.c_str(), fg, bg, limit);
    }

    // The desktop: flat purple with a faint grid, the background everything else
    // sits on.
    void desktop() {
        clear(pal::desktop);
        for (int y = 0; y < h_; y += 2) {
            for (int x = 0; x < w_; x += 4) set(x, y, "\xc2\xb7", pal::grid, pal::desktop); // U+00B7
        }
    }

    // A chunky window with a solid title bar and a hard drop shadow, two columns
    // right and one row down. Returns the usable interior.
    Rect window(const Rect& r, const char* title, Rgb bar) {
        // Shadow first, so the frame paints over its own corner.
        for (int y = r.y + 1; y < r.bottom() + 1; ++y) {
            for (int x = r.right(); x < r.right() + 2; ++x) set(x, y, " ", pal::shadow, pal::shadow);
        }
        for (int x = r.x + 2; x < r.right() + 2; ++x) set(x, r.bottom(), " ", pal::shadow, pal::shadow);

        fill(r, pal::panel);
        // Title bar, with the name in dark ink on the hot colour.
        const Rect tb{r.x, r.y, r.w, 1};
        fill(tb, bar);
        text(r.x + 1, r.y, title, pal::title_ink, bar, r.w - 4);
        // The three chunky buttons every one of these had.
        if (r.w > 8) text(r.right() - 4, r.y, "\xe2\x96\xa0\xe2\x96\xa0\xe2\x96\xa0", pal::title_ink, bar);

        for (int y = r.y + 1; y < r.bottom(); ++y) {
            set(r.x, y, "\xe2\x94\x82", pal::panel_edge, pal::panel);
            set(r.right() - 1, y, "\xe2\x94\x82", pal::panel_edge, pal::panel);
        }
        for (int x = r.x; x < r.right(); ++x) set(x, r.bottom() - 1, "\xe2\x94\x80", pal::panel_edge, pal::panel);
        return Rect{r.x + 2, r.y + 1, r.w - 4, r.h - 2};
    }

    // A horizontal meter in eighth-blocks, so a bar has eight times the
    // resolution of its column count.
    void bar(int x, int y, int w, double frac, Rgb on, Rgb bg = pal::bar_bg) {
        if (w <= 0) return;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        const int eighths = static_cast<int>(frac * static_cast<double>(w) * 8.0 + 0.5);
        const int full = eighths / 8;
        const int rest = eighths % 8;
        for (int i = 0; i < w; ++i) {
            if (i < full) {
                set(x + i, y, "\xe2\x96\x88", on, bg); // U+2588 full block
            } else if (i == full && rest > 0) {
                set(x + i, y, kEighths[rest], on, bg);
            } else {
                set(x + i, y, " ", bg, bg);
            }
        }
    }

    // A meter that runs both ways from a centre line: the shape a signed number
    // wants, and the one the P&L column uses.
    void signed_bar(int x, int y, int w, double frac, Rgb pos = pal::up, Rgb neg = pal::down) {
        if (w < 3) return;
        const int mid = w / 2;
        for (int i = 0; i < w; ++i) set(x + i, y, " ", pal::bar_bg, pal::bar_bg);
        set(x + mid, y, "\xe2\x94\x82", pal::dim, pal::bar_bg);
        if (frac > 1) frac = 1;
        if (frac < -1) frac = -1;
        const int n = static_cast<int>((frac < 0 ? -frac : frac) * static_cast<double>(mid) + 0.5);
        for (int i = 0; i < n; ++i) {
            const int cx = frac >= 0 ? mid + 1 + i : mid - 1 - i;
            if (cx >= 0 && cx < w) set(x + cx, y, "\xe2\x96\x88", frac >= 0 ? pos : neg, pal::bar_bg);
        }
    }

    // A labelled slider: the control the reaction-delay window is made of.
    void slider(int x, int y, int w, double frac, Rgb knob) {
        if (w <= 0) return;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        for (int i = 0; i < w; ++i) set(x + i, y, "\xe2\x94\x80", pal::dim, pal::panel);
        const int at = static_cast<int>(frac * static_cast<double>(w - 1) + 0.5);
        set(x + at, y, "\xe2\x96\x88", knob, pal::panel);
    }

    // The modal box, for the one thing worth interrupting a frame about.
    void dialog(const Rect& r, const char* title, const char* const* lines, int count) {
        const Rect in = window(r, title, pal::magenta);
        for (int i = 0; i < count && i < in.h; ++i) text(in.x, in.y + i, lines[i], pal::ink, pal::panel, in.w);
    }

    // ---- reading the grid back, for tests -------------------------------
    //
    // The renderer is the one part of the arena that makes no measured claim, so
    // what is worth pinning is not how it looks but the invariants a layout edit
    // could silently break: one cell per code point, clipping at the edges, and
    // a frame that composes identically every time. test_arena asserts those
    // through these three accessors and a hash of compose().

    [[nodiscard]] std::string_view glyph_at(int x, int y) const {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return {};
        const Cell& c = cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) +
                               static_cast<std::size_t>(x)];
        return std::string_view(c.g, c.len);
    }

    [[nodiscard]] Rgb fg_at(int x, int y) const {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return Rgb{};
        return cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) +
                      static_cast<std::size_t>(x)]
            .fg;
    }

    [[nodiscard]] Rgb bg_at(int x, int y) const {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return Rgb{};
        return cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) +
                      static_cast<std::size_t>(x)]
            .bg;
    }

    // One frame, one write. Colour escapes only where a colour changes.
    void present(std::FILE* f) {
        const std::string& frame = compose();
        std::fwrite(frame.data(), 1, frame.size(), f);
        std::fflush(f);
    }

    // The bytes present() would write. Split out so a test can hash a frame
    // without a terminal, and so present() stays one write.
    const std::string& compose() {
        out_.assign("\x1b[H");
        Rgb fg{1, 2, 3}, bg{4, 5, 6}; // impossible, so the first cell emits both
        char esc[48];
        for (int y = 0; y < h_; ++y) {
            for (int x = 0; x < w_; ++x) {
                const Cell& c =
                    cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)];
                if (!(c.fg == fg)) {
                    std::snprintf(esc, sizeof esc, "\x1b[38;2;%u;%u;%um", c.fg.r, c.fg.g, c.fg.b);
                    out_ += esc;
                    fg = c.fg;
                }
                if (!(c.bg == bg)) {
                    std::snprintf(esc, sizeof esc, "\x1b[48;2;%u;%u;%um", c.bg.r, c.bg.g, c.bg.b);
                    out_ += esc;
                    bg = c.bg;
                }
                out_.append(c.g, c.len);
            }
            if (y + 1 < h_) out_ += "\x1b[0m\r\n";
            bg = Rgb{4, 5, 6};
            fg = Rgb{1, 2, 3};
        }
        out_ += "\x1b[0m";
        return out_;
    }

private:
    // Bytes in the UTF-8 sequence starting with `b`. A stray continuation byte
    // counts as one, so a malformed string advances rather than looping.
    [[nodiscard]] static int utf8_len(unsigned char b) noexcept {
        if (b < 0x80) return 1;
        if ((b & 0xE0) == 0xC0) return 2;
        if ((b & 0xF0) == 0xE0) return 3;
        if ((b & 0xF8) == 0xF0) return 4;
        return 1;
    }

    struct Cell {
        char g[4] = {' ', 0, 0, 0};
        std::uint8_t len = 1;
        Rgb fg = pal::ink;
        Rgb bg = pal::desktop;
    };

    // U+258F..U+2589: one-eighth through seven-eighths blocks, index 1..7.
    static constexpr const char* kEighths[8] = {" ",
                                                "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d", "\xe2\x96\x8c",
                                                "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89"};

    int w_ = 0, h_ = 0;
    std::vector<Cell> cells_;
    std::string out_;
};

} // namespace px::arena::ui
