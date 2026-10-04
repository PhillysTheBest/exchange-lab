// arena/arena.cpp — watch a session happen.
//
// The retro desktop: an order book ladder, the tape, the participants and what
// they are worth, the reaction-delay sliders that are the experiment's dial, and
// a dialog that interrupts when something notable happens. This is the only file
// where the renderer (arena/ui.hpp) and the market (arena/session.hpp) meet; the
// first knows nothing about trading and the second knows nothing about drawing.
//
// IT CANNOT CHANGE WHAT IT WATCHES. The viewer paces a session by calling
// run_until() with a simulated timestamp, and the keyboard only ever changes how
// fast it asks for the next one. Pause, speed, focus and quit do not inject
// orders, do not touch the clock a session reads, and cannot move the session
// hash: a session watched at a hundred times speed, paused in the middle and
// resumed, produces the same hash as the same seed run headless in the sweep.
// That is the point of the viewer being a reader. It is also what settles the
// plan's open question about trading by hand: live keystrokes would be a
// wall-clock read inside a session, which would break replay, so the human gets
// the controls and the market gets none of them.
//
//   space   pause and resume            1-9  focus a participant
//   + -     faster, slower              q    quit
//
// --still runs a session headless to one simulated instant and prints a single
// frame, which makes a screenshot reproducible from a seed.
#include "arena/session.hpp"
#include "arena/term.hpp"
#include "arena/ui.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace px;
using namespace px::arena;
using px::arena::ui::Rect;
using px::arena::ui::Rgb;
namespace pal = px::arena::ui::pal;

namespace {

constexpr int kMinCols = 96;
constexpr int kMinRows = 32;

struct ViewOptions {
    std::uint64_t seed = 42;
    std::size_t requests = 0;             // 0 = until the clock runs out
    SimTime still_at_ns = 0;              // >0: render one frame and exit
    double sim_ns_per_second = 2'000'000; // 2 ms of market per real second
    SimTime maker_reaction_ns = -1;       // <0 = the registry's default
};

std::string fmt_price(Price p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld.%02lld", static_cast<long long>(p / kPriceScale),
                  static_cast<long long>((p % kPriceScale) / 100));
    return buf;
}

std::string fmt_money(std::int64_t price_units) {
    char buf[32];
    const double v = static_cast<double>(price_units) / static_cast<double>(kPriceScale);
    std::snprintf(buf, sizeof buf, "%+.2f", v);
    return buf;
}

std::string fmt_ns(SimTime ns) {
    char buf[32];
    if (ns < 1'000) {
        std::snprintf(buf, sizeof buf, "%lldns", static_cast<long long>(ns));
    } else if (ns < 1'000'000) {
        std::snprintf(buf, sizeof buf, "%.3gus", static_cast<double>(ns) / 1e3);
    } else {
        std::snprintf(buf, sizeof buf, "%.3gms", static_cast<double>(ns) / 1e6);
    }
    return buf;
}

// ---- the windows ---------------------------------------------------------

// The ladder, asks above and bids below, meeting at the middle line. Size is a
// bar, so depth is a shape rather than a column of numbers. Drawn from the
// engine itself: this is the exchange's own view, not any participant's.
void draw_book(ui::Screen& s, const Rect& r, const Session& sess) {
    const Rect in = s.window(r, "ORDER BOOK", pal::teal);
    const MatchingEngine& eng = sess.engine();
    const InstrumentId id = sess.instrument();
    const int rows = in.h - 1;
    const int per_side = rows / 2;
    if (per_side < 1) return;

    std::uint64_t biggest = 1;
    for (int i = 0; i < per_side; ++i) {
        for (const Side sd : {Side::Buy, Side::Sell}) {
            if (static_cast<std::uint32_t>(i) < eng.depth(id, sd)) {
                const std::uint64_t q = eng.level(id, sd, static_cast<std::uint32_t>(i)).qty;
                if (q > biggest) biggest = q;
            }
        }
    }

    const int bar_x = in.x + 20;
    const int bar_w = in.w - 21;
    // Asks: worst at the top, best just above the middle line.
    for (int i = 0; i < per_side; ++i) {
        const int rank = per_side - 1 - i;
        const int y = in.y + i;
        if (static_cast<std::uint32_t>(rank) >= eng.depth(id, Side::Sell)) continue;
        const LevelView l = eng.level(id, Side::Sell, static_cast<std::uint32_t>(rank));
        s.text(in.x, y, fmt_price(l.price), pal::down, pal::panel, 10);
        char q[16];
        std::snprintf(q, sizeof q, "%8llu", static_cast<unsigned long long>(l.qty));
        s.text(in.x + 10, y, q, pal::dim, pal::panel, 9);
        s.bar(bar_x, y, bar_w, static_cast<double>(l.qty) / static_cast<double>(biggest), pal::down);
    }

    const int mid_y = in.y + per_side;
    for (int x = in.x; x < in.x + in.w; ++x) s.set(x, mid_y, "\xe2\x94\x80", pal::panel_edge, pal::panel);
    const Price fv = sess.fair_value().value();
    s.text(in.x, mid_y, " value " + fmt_price(fv) + " ", pal::ink, pal::panel, in.w);

    for (int i = 0; i < per_side; ++i) {
        const int y = mid_y + 1 + i;
        if (static_cast<std::uint32_t>(i) >= eng.depth(id, Side::Buy)) continue;
        const LevelView l = eng.level(id, Side::Buy, static_cast<std::uint32_t>(i));
        s.text(in.x, y, fmt_price(l.price), pal::up, pal::panel, 10);
        char q[16];
        std::snprintf(q, sizeof q, "%8llu", static_cast<unsigned long long>(l.qty));
        s.text(in.x + 10, y, q, pal::dim, pal::panel, 9);
        s.bar(bar_x, y, bar_w, static_cast<double>(l.qty) / static_cast<double>(biggest), pal::up);
    }
}

void draw_tape(ui::Screen& s, const Rect& r, const Session& sess) {
    const Rect in = s.window(r, "TAPE", pal::magenta);
    const std::vector<TapePrint>& tape = sess.tape();
    for (int i = 0; i < in.h; ++i) {
        const std::size_t idx = tape.size() - 1 - static_cast<std::size_t>(i);
        if (static_cast<std::size_t>(i) >= tape.size()) break;
        const TapePrint& t = tape[idx];
        const bool bought = t.aggressor == Side::Buy;
        const int y = in.y + i;
        s.text(in.x, y, bought ? "\xe2\x96\xb2" : "\xe2\x96\xbc", bought ? pal::up : pal::down, pal::panel);
        s.text(in.x + 2, y, fmt_price(t.price), bought ? pal::up : pal::down, pal::panel, 9);
        char q[24];
        std::snprintf(q, sizeof q, "%5u", t.qty);
        s.text(in.x + 11, y, q, pal::ink, pal::panel, 6);
        s.text(in.x + 17, y, sess.name_of(t.taker), pal::dim, pal::panel, in.w - 17);
    }
}

// One row per participant: what it is, what it holds, and what it is worth as a
// bar that runs both ways from zero.
void draw_participants(ui::Screen& s, const Rect& r, const Session& sess, int focus) {
    const Rect in = s.window(r, "PARTICIPANTS", pal::violet);
    const Price mark = sess.fair_value().value();
    std::int64_t worst = 1;
    for (std::size_t i = 0; i < sess.participants(); ++i) {
        const std::int64_t v = sess.ledger().at(i).value(mark);
        const std::int64_t m = v < 0 ? -v : v;
        if (m > worst) worst = m;
    }
    s.text(in.x, in.y, "  participant  delay    pos     value", pal::dim, pal::panel, in.w);
    for (std::size_t i = 0; i < sess.participants() && static_cast<int>(i) + 1 < in.h; ++i) {
        const int y = in.y + 1 + static_cast<int>(i);
        const Account& a = sess.ledger().at(i);
        const bool on = static_cast<int>(i) == focus;
        const Rgb fg = on ? pal::ink : pal::dim;
        if (on) {
            for (int x = in.x; x < in.x + in.w; ++x) s.set(x, y, " ", fg, pal::bar_bg);
        }
        const Rgb bg = on ? pal::bar_bg : pal::panel;
        char line[128];
        std::snprintf(line, sizeof line, "%d %-9s %6s %6lld %9s", static_cast<int>(i) + 1, sess.name_of(i),
                      fmt_ns(sess.reaction_of(i)).c_str(), static_cast<long long>(a.position),
                      fmt_money(a.value(mark)).c_str());
        s.text(in.x, y, line, sess.informed(i) ? pal::orange : fg, bg, in.w - 8);
        s.signed_bar(in.x + in.w - 7, y, 7, static_cast<double>(a.value(mark)) / static_cast<double>(worst));
    }
}

// The dial the experiment turns. Each participant's reaction delay on a log
// slider, because the interesting range spans a thousand.
void draw_reaction(ui::Screen& s, const Rect& r, const Session& sess, int focus) {
    const Rect in = s.window(r, "REACTION DELAY", pal::orange);
    s.text(in.x, in.y, "simulated, not measured", pal::dim, pal::panel, in.w);
    for (std::size_t i = 0; i < sess.participants() && static_cast<int>(i) + 2 < in.h; ++i) {
        const int y = in.y + 2 + static_cast<int>(i);
        const SimTime d = sess.reaction_of(i);
        // 1 us at the left, 1 ms at the right, log scaled.
        double frac = 0;
        if (d > 0) {
            const double lo = 3.0, hi = 6.0; // log10 ns
            double l = 0;
            for (SimTime v = d; v >= 10; v /= 10) l += 1.0;
            l += static_cast<double>(d) / 1e18; // keeps equal decades distinct
            frac = (l - lo) / (hi - lo);
        }
        const Rgb knob = static_cast<int>(i) == focus ? pal::orange : pal::dim;
        s.text(in.x, y, sess.name_of(i), static_cast<int>(i) == focus ? pal::ink : pal::dim, pal::panel, 10);
        s.slider(in.x + 10, y, in.w - 19, frac, knob);
        s.text(in.x + in.w - 8, y, fmt_ns(d), knob, pal::panel, 8);
    }
}

void draw_session(ui::Screen& s, const Rect& r, const Session& sess, const ViewOptions& opt, bool paused,
                  double speed) {
    const Rect in = s.window(r, "SESSION", pal::teal);
    const SessionStats& st = sess.stats();
    char line[160];
    std::snprintf(line, sizeof line, "seed %llu   clock %s   %s", static_cast<unsigned long long>(opt.seed),
                  fmt_ns(sess.now()).c_str(), paused ? "PAUSED" : "running");
    s.text(in.x, in.y, line, pal::ink, pal::panel, in.w);
    std::snprintf(line, sizeof line, "%zu orders, %llu trades", st.requests,
                  static_cast<unsigned long long>(st.trades));
    s.text(in.x, in.y + 1, line, pal::dim, pal::panel, in.w);
    std::snprintf(line, sizeof line, "book-value gap %.1f tk (max %llu)", st.mean_gap_ticks(),
                  static_cast<unsigned long long>(st.gap_max_ticks));
    s.text(in.x, in.y + 2, line, pal::dim, pal::panel, in.w);
    if (speed > 0) {
        std::snprintf(line, sizeof line, "speed %.0fx of simulated time", speed);
        s.text(in.x, in.y + 3, line, pal::dim, pal::panel, in.w);
    }
    if (in.h > 4) {
        const double frac = opt.requests == 0
                                ? 0.0
                                : static_cast<double>(st.requests) / static_cast<double>(opt.requests);
        if (opt.requests != 0) s.bar(in.x, in.y + 4, in.w, frac, pal::teal);
    }
}

// The one thing worth interrupting a frame for: a print that cleared several
// levels at once. Held on screen briefly, in simulated time, so that pausing
// does not make it disappear.
struct Alert {
    std::string line1, line2;
    SimTime until = 0;
    [[nodiscard]] bool live(SimTime now) const noexcept { return until > now && !line1.empty(); }
};

void draw_alert(ui::Screen& s, const Rect& r, const Alert& a) {
    const char* lines[2] = {a.line1.c_str(), a.line2.c_str()};
    s.dialog(r, "! NOTABLE", lines, 2);
}

void compose(ui::Screen& s, const Session& sess, const ViewOptions& opt, int focus, bool paused, double speed,
             const Alert& alert) {
    s.desktop();
    const int w = s.width(), h = s.height();
    const int left_w = w * 40 / 100;
    const int right_x = left_w + 4;
    const int right_w = w - right_x - 3;

    s.text(2, 0, " ARENA \xe2\x94\x82 bots trading inside the matching engine ", pal::ink, pal::grid);

    const int book_h = h - 12;
    draw_book(s, Rect{2, 2, left_w, book_h}, sess);
    draw_session(s, Rect{2, 2 + book_h + 1, left_w, 7}, sess, opt, paused, speed);

    const int part_h = static_cast<int>(sess.participants()) + 4;
    draw_participants(s, Rect{right_x, 2, right_w, part_h}, sess, focus);
    const int react_y = 2 + part_h + 1;
    const int react_h = static_cast<int>(sess.participants()) + 5;
    draw_reaction(s, Rect{right_x, react_y, right_w, react_h}, sess, focus);
    const int tape_y = react_y + react_h + 1;
    const int tape_h = h - tape_y - 3;
    if (tape_h > 3) draw_tape(s, Rect{right_x, tape_y, right_w, tape_h}, sess);

    s.text(2, h - 1, "space pause   + - speed   1-9 focus   q quit", pal::dim, pal::desktop, w - 4);

    if (alert.live(sess.now())) {
        const int dw = 46, dh = 5;
        draw_alert(s, Rect{(w - dw) / 2, (h - dh) / 2, dw, dh}, alert);
    }
}

// Watches the tape for a print that took out several levels, which is the kind
// of thing worth a dialog. Reads the session rather than being told by it, so
// the session carries no notion of what is interesting.
Alert detect_alert(const Session& sess, std::size_t& seen_trades, Price& last_mid) {
    Alert a;
    const std::vector<TapePrint>& tape = sess.tape();
    if (tape.empty()) return a;
    if (sess.stats().trades <= seen_trades) return a;
    seen_trades = sess.stats().trades;
    const Price bid = sess.engine().depth(sess.instrument(), Side::Buy) == 0
                          ? 0
                          : sess.engine().level(sess.instrument(), Side::Buy, 0).price;
    const Price ask = sess.engine().depth(sess.instrument(), Side::Sell) == 0
                          ? 0
                          : sess.engine().level(sess.instrument(), Side::Sell, 0).price;
    if (bid == 0 || ask == 0) return a;
    const Price mid = (bid + ask) / 2;
    if (last_mid == 0) {
        last_mid = mid;
        return a;
    }
    const Price jump = mid > last_mid ? mid - last_mid : last_mid - mid;
    const Price tick = sess.config().tick;
    if (jump >= 4 * tick) {
        const TapePrint& t = tape.back();
        a.line1 = std::string(sess.name_of(t.taker)) + " swept " + fmt_price(t.price) + ", mid moved " +
                  std::to_string(static_cast<long long>(jump / tick)) + " ticks";
        a.line2 = "maker on the other side: " + std::string(sess.name_of(t.maker));
        a.until = sess.now() + 400'000;
    }
    last_mid = mid;
    return a;
}

} // namespace

int main(int argc, char** argv) {
    ViewOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--help" || a == "-h") {
            std::printf("arena — watch a session happen.\n\n"
                        "  --seed=N             session seed (default 42)\n"
                        "  --requests=N         stop after N engine requests (default: run on)\n"
                        "  --speed=NS           simulated ns per real second (default 2000000)\n"
                        "  --maker-reaction=NS  the swept maker's reaction delay\n"
                        "  --still=NS           render one frame at that simulated instant and exit\n\n"
                        "keys: space pause, + - speed, 1-9 focus, q quit\n\n"
                        "The viewer only reads the session. Pausing, speeding up and focusing cannot\n"
                        "change what happens, so a watched session has the same hash as a headless one.\n"
                        "Reaction delays are simulated inputs; the engine's measured latency is in README.md.\n");
            return 0;
        }
        const auto num = [&](std::string_view pfx, long long& out) {
            if (!a.starts_with(pfx)) return false;
            out = std::atoll(std::string(a.substr(pfx.size())).c_str());
            return true;
        };
        long long v = 0;
        if (num("--seed=", v)) {
            opt.seed = static_cast<std::uint64_t>(v);
        } else if (num("--requests=", v)) {
            opt.requests = static_cast<std::size_t>(v);
        } else if (num("--still=", v)) {
            opt.still_at_ns = v;
        } else if (num("--speed=", v)) {
            if (v > 0) opt.sim_ns_per_second = static_cast<double>(v);
        } else if (num("--maker-reaction=", v)) {
            opt.maker_reaction_ns = v;
        } else {
            std::fprintf(stderr, "unknown argument: %.*s (try --help)\n", static_cast<int>(a.size()), a.data());
            return 2;
        }
    }

    SessionConfig sc;
    sc.seed = opt.seed;
    sc.max_requests = opt.requests;
    RosterConfig rc;
    if (opt.maker_reaction_ns >= 0) rc.maker_reaction_ns = opt.maker_reaction_ns;

    Session sess;
    sess.start(sc, rc);

    // --still: no terminal setup, no input, no pacing. One frame from a seed,
    // which is what makes a screenshot in the documentation reproducible.
    if (opt.still_at_ns > 0) {
        sess.run_until(opt.still_at_ns);
        const TermSize ts = Terminal::size();
        ui::Screen screen(ts.cols < kMinCols ? kMinCols : ts.cols, ts.rows < kMinRows ? kMinRows : ts.rows);
        std::size_t seen = 0;
        Price last_mid = 0;
        const Alert alert = detect_alert(sess, seen, last_mid);
        compose(screen, sess, opt, 0, true, 0, alert);
        screen.present(stdout);
        std::printf("\x1b[0m\n");
        std::printf("still at %s, seed %llu, session hash %016llx\n", fmt_ns(sess.now()).c_str(),
                    static_cast<unsigned long long>(opt.seed), static_cast<unsigned long long>(sess.hash()));
        return 0;
    }

    Terminal term;
    if (!term.colour_ok()) {
        std::fprintf(stderr, "this terminal will not interpret 24-bit colour escapes.\n"
                             "Try Windows Terminal, or run `arena --still=2000000` for one frame.\n");
        return 1;
    }
    TermSize ts = Terminal::size();
    if (ts.cols < kMinCols || ts.rows < kMinRows) {
        std::fprintf(stderr, "the desktop needs at least %dx%d; this terminal is %dx%d.\n", kMinCols, kMinRows,
                     ts.cols, ts.rows);
        return 1;
    }
    ui::Screen screen(ts.cols, ts.rows);

    bool paused = false;
    int focus = 0;
    double speed = 1.0;
    std::size_t seen_trades = 0;
    Price last_mid = 0;
    Alert alert;
    auto last = std::chrono::steady_clock::now();
    const auto frame = std::chrono::milliseconds(33);

    while (true) {
        const int key = Terminal::poll_key();
        if (key == 'q' || key == 'Q' || key == 3 || key == 27) break;
        if (key == ' ') paused = !paused;
        if (key == '+' || key == '=') speed = speed < 512 ? speed * 2 : speed;
        if (key == '-' || key == '_') speed = speed > 1.0 / 64 ? speed / 2 : speed;
        if (key >= '1' && key <= '9') {
            const int want = key - '1';
            if (static_cast<std::size_t>(want) < sess.participants()) focus = want;
        }

        const auto now = std::chrono::steady_clock::now();
        const double real_s = std::chrono::duration<double>(now - last).count();
        last = now;

        if (!paused && !sess.done()) {
            // The only coupling between real time and simulated time: how far
            // ahead the viewer asks the session to go. The session's own clock
            // is untouched by how long that took.
            const auto advance = static_cast<SimTime>(opt.sim_ns_per_second * speed * real_s);
            sess.run_until(sess.now() + (advance < 1 ? 1 : advance));
            const Alert fresh = detect_alert(sess, seen_trades, last_mid);
            if (fresh.live(sess.now())) alert = fresh;
        }

        const TermSize sz = Terminal::size();
        if (sz.cols != ts.cols || sz.rows != ts.rows) {
            ts = sz;
            if (ts.cols >= kMinCols && ts.rows >= kMinRows) screen.resize(ts.cols, ts.rows);
        }
        compose(screen, sess, opt, focus, paused, speed, alert);
        screen.present(stdout);
        std::this_thread::sleep_for(frame);
    }

    sess.finish();
    std::printf("\x1b[0m\nseed %llu, %zu orders, %llu trades, session hash %016llx\n",
                static_cast<unsigned long long>(opt.seed), sess.stats().requests,
                static_cast<unsigned long long>(sess.stats().trades),
                static_cast<unsigned long long>(sess.hash()));
    return 0;
}
