#!/usr/bin/env python3
"""examples/frozen/train.py: train a tiny model outside exchange-lab and freeze it.

    build/release/calibrate --known=known          # the training tape: data/known.tape
    python3 examples/frozen/train.py data/known.tape > examples/strategies/frozen.hpp

It reads a tape (docs/DATA.md), rebuilds the top of the book order by order, and fits
a linear model of where the mid goes next from three things a strategy can see at a
touch change. It then rounds the weights to integers and writes them into a C++
header that follows the strategy convention, so the model runs inside a session like
any other strategy: integer arithmetic only, no model call, no file read, no clock.

Why least squares and not something larger: the point is the path from "trained
elsewhere" to "a pure integer function of the market view", not the model. Why least
squares in particular: it is solved with + - * / only, which IEEE 754 rounds exactly,
so running this script again gives the same weights on any machine. A sigmoid would
need exp(), which a maths library may round differently.

Standard library only. With --check it regenerates the header and compares it with
the one committed, byte for byte.
"""
import sys
from pathlib import Path

HORIZON = 5        # the label: how far the mid moves over the next 5 prints
SCALE = 1 << 16    # weights are frozen as integers, times 2^16
RIDGE = 1e-6       # a little ridge, so a feature that never moves cannot make it singular


def cksum(data: bytes) -> str:
    """POSIX cksum: CRC-32 (0x04C11DB7) over the bytes and then their length."""
    crc = 0
    def feed(b):
        nonlocal crc
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    for b in data:
        feed(b)
    n = len(data)
    while n:
        feed(n & 0xFF)
        n >>= 8
    return f"{(~crc) & 0xFFFFFFFF} {len(data)}"


def tdiv(a: int, b: int) -> int:
    """Integer division that truncates toward zero, as C++ does."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def features(bid, bq, ask, aq, last):
    """What the strategy computes at a wake, in the same integers (prices in ticks)."""
    imbalance = tdiv((bq - aq) * 1000, bq + aq)        # -1000 .. 1000
    where = 0 if last == 0 else 2 * last - (bid + ask)  # the last print: + at the ask, - at the bid
    spread = ask - bid
    return [imbalance, where, spread]


def read_samples(path: Path):
    """(features, label) at every moment the touch changed, label in half ticks."""
    lines = path.read_text(encoding="utf-8").splitlines()
    head = lines.index("t_ns,kind,side,id,price,qty")
    header = dict(l.split("=", 1) for l in lines[:head] if "=" in l and not l.startswith("#"))
    if header.get("depth") != "L3":
        sys.exit(f"{path}: the training tape must be order by order (L3), not {header.get('depth')}")
    orders, levels = {}, [{}, {}]
    def add(side, price, qty):
        lv = levels[side]
        lv[price] = lv.get(price, 0) + qty
        if lv[price] <= 0:
            del lv[price]
    def take(oid, qty):
        if oid not in orders:
            return
        side, price, left = orders[oid]
        d = min(qty, left)
        add(side, price, -d)
        if left - d == 0:
            del orders[oid]
        else:
            orders[oid] = (side, price, left - d)

    segments, moments = [], []   # moments: (top, last print, prints so far) per moment
    prints, last, prev_top = 0, 0, None
    rows = [l.split(",") for l in lines[head + 1:]]
    i = 0
    while i < len(rows):
        t = rows[i][0]
        traded = False
        while i < len(rows) and rows[i][0] == t:
            _, kind, side, oid, price, qty = rows[i]
            oid, price, qty = int(oid), int(price), int(qty)
            s = 0 if side == "B" else 1
            if kind == "R":
                segments.append(moments)
                moments, orders, levels, prev_top, last = [], {}, [{}, {}], None, 0
            elif kind == "A":
                orders[oid] = (s, price, qty)
                add(s, price, qty)
            elif kind == "C":
                take(oid, qty)
            elif kind == "T":
                take(oid, qty)
                last, traded = price, True
            i += 1
        prints += 1 if traded else 0
        if levels[0] and levels[1]:
            bid, ask = max(levels[0]), min(levels[1])
            top = (bid, levels[0][bid], ask, levels[1][ask])
            if ask > bid:
                moments.append((top, last, prints, top != prev_top))
            prev_top = top
    segments.append(moments)

    samples = []
    for ms in segments:
        for k, (top, lst, p, changed) in enumerate(ms):
            if not changed:
                continue
            # The first moment at least HORIZON prints later, in the same session.
            j = k + 1
            while j < len(ms) and ms[j][2] < p + HORIZON:
                j += 1
            if j == len(ms):
                break
            bid, bq, ask, aq = top
            fut = ms[j][0]
            samples.append((features(bid, bq, ask, aq, lst), (fut[0] + fut[2]) - (bid + ask)))
    return header, samples


def solve(a, b):
    """Gaussian elimination with partial pivoting: + - * / only."""
    n = len(b)
    m = [row[:] + [b[r]] for r, row in enumerate(a)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(m[r][c]))
        m[c], m[p] = m[p], m[c]
        for r in range(n):
            if r != c:
                f = m[r][c] / m[c][c]
                m[r] = [x - f * y for x, y in zip(m[r], m[c])]
    return [m[r][n] / m[r][r] for r in range(n)]


def fit(samples):
    x = [[1.0] + [float(v) for v in f] for f, _ in samples]
    y = [float(l) for _, l in samples]
    k = len(x[0])
    xtx = [[sum(r[i] * r[j] for r in x) + (RIDGE * len(x) if i == j and i > 0 else 0.0) for j in range(k)] for i in range(k)]
    xty = [sum(r[i] * t for r, t in zip(x, y)) for i in range(k)]
    w = solve(xtx, xty)
    mean = sum(y) / len(y)
    sst = sum((t - mean) ** 2 for t in y)
    sse = sum((t - sum(wi * xi for wi, xi in zip(w, r))) ** 2 for r, t in zip(x, y))
    return w, 1.0 - sse / sst


def frozen(w):
    """Round half away from zero, with floor only."""
    out = []
    for v in w:
        s = v * SCALE
        out.append(int(s + 0.5) if s >= 0 else -int(-s + 0.5))
    return out


HEADER = """\
// examples/strategies/frozen.hpp - a model trained elsewhere, frozen into integers.
//
// GENERATED by examples/frozen/train.py; do not edit by hand. Regenerate with
//     build/release/calibrate --known=known
//     python3 examples/frozen/train.py data/known.tape > examples/strategies/frozen.hpp
// and check with --check. examples/frozen/README.md says how it was made and why.
//
// What was trained. A linear model of how far the mid moves over the next {horizon} prints,
// in half ticks, from three things visible at a touch change: the touch imbalance
// (bid size - ask size, per 1000 of both), where the last print was against the mid
// (+ at the ask, - at the bid, in half ticks) and the spread in ticks. Least squares on
// {n} touch changes of the tape below; R^2 {r2} on the data it was fitted to, so
// it explains little, and nothing here says it predicts anything out of sample.
//
// Trained on:  tape {tape} ({tape_cksum}, cksum), {tape_note}
// Trained by:  examples/frozen/train.py ({script_cksum}, cksum)
// The tape is the known-answer control's (docs/DATA.md): a market of the same model with
// other parameters, on seeds 7001-7008, which no block a strategy is scored on uses.
// `make canonical` pins its checksum (tape.known), so the training data is checkable too.
//
// How it trades. It rests one order at the touch, on the side the model says the mid is
// going: the best bid when it predicts a rise, the best ask when it predicts a fall, and
// nothing when it predicts no move or a side is empty. It pulls the order when the
// prediction turns or the touch moves away, within a position budget. Its predictions
// are smaller than the half spread, so taking liquidity on them would never pay; resting
// earns the spread and lets the model choose only which side. Like imbalance.hpp it is an
// example, not a recommendation.
//
// Integer only. The weights are the fitted ones times 2^16, rounded; the score is the
// predicted move in half ticks times 2^16. No model is called, nothing is read at run
// time, and the run records this header's cksum (docs/REPRODUCE.md), so the model a
// result came from is the model in this file, byte for byte.
#pragma once

#include "arena/strategy.hpp"

namespace px::arena::strategies {{

struct Frozen {{
    const char* name = "frozen";

    SimTime reaction_ns = 10'000;
    SimTime timer_ns = 0; // event-driven, like the touch changes it was trained on

    // The frozen model: predicted move of the mid, half ticks x 2^16.
    static constexpr std::int64_t kScale = {scale};
    static constexpr std::int64_t kBias = {w0};
    static constexpr std::int64_t kImbalance = {w1}; // per unit of imbalance (per mille)
    static constexpr std::int64_t kLastPrint = {w2}; // per half tick of the last print against the mid
    static constexpr std::int64_t kSpread = {w3};    // per tick of spread

    Qty qty = 10;
    std::int64_t max_position = 200;

    void reset(std::uint64_t) {{}}

    // The model's prediction at this view, half ticks x 2^16; 0 with a side empty.
    [[nodiscard]] static std::int64_t score(const MarketView& v) {{
        const Price bid = v.best(Side::Buy), ask = v.best(Side::Sell);
        const std::int64_t bq = static_cast<std::int64_t>(v.size_at_best(Side::Buy));
        const std::int64_t aq = static_cast<std::int64_t>(v.size_at_best(Side::Sell));
        if (bid == 0 || ask == 0 || ask <= bid || bq + aq == 0) return 0;
        const std::int64_t imbalance = (bq - aq) * 1000 / (bq + aq);
        const Price last = v.last_trade();
        const std::int64_t where = last == 0 ? 0 : (2 * last - bid - ask) / v.tick;
        const std::int64_t spread = (ask - bid) / v.tick;
        return kBias + kImbalance * imbalance + kLastPrint * where + kSpread * spread;
    }}

    ActionBatch on_event(const MarketView& v) {{
        ActionBatch out;
        if (v.in_flight != 0) return out;
        const std::int64_t s = score(v);
        const Side want = s > 0 ? Side::Buy : Side::Sell;
        const bool room = want == Side::Buy ? v.position + qty <= max_position : v.position - qty >= -max_position;
        const bool quote = s != 0 && room;
        bool have = false;
        for (std::size_t i = 0; i < v.my_order_count; ++i) {{
            const BookMirror::OrderRef& o = v.my_orders[i];
            if (!quote || o.side != want || o.price != v.best(want) || have) out.push(Action::cancel(o.id));
            else have = true;
        }}
        if (quote && !have) out.push(Action::limit(want, v.best(want), qty));
        return out;
    }}
}};

}} // namespace px::arena::strategies
"""


def main():
    args = [a for a in sys.argv[1:] if a != "--check"]
    check = "--check" in sys.argv[1:]
    if len(args) != 1:
        sys.exit("usage: python3 examples/frozen/train.py [--check] data/known.tape")
    tape = Path(args[0])
    header, samples = read_samples(tape)
    if len(samples) < 1000:
        sys.exit(f"{tape}: {len(samples)} touch changes is too few to fit anything")
    w, r2 = fit(samples)
    q = frozen(w)
    script = Path(__file__)
    text = HEADER.format(
        horizon=HORIZON, n=len(samples), r2=f"{r2:.3f}", tape=header.get("name", "?"),
        tape_cksum=cksum(tape.read_bytes()), tape_note=header.get("notes", "").split(":")[0],
        script_cksum=cksum(script.read_bytes()), scale=SCALE, w0=q[0], w1=q[1], w2=q[2], w3=q[3])
    if check:
        committed = (script.parent.parent / "strategies" / "frozen.hpp").read_bytes().decode("utf-8")
        same = committed == text
        print("frozen.hpp: " + ("identical to what this script writes from " + str(tape) if same
                                else "DIFFERENT from what this script writes; regenerate it"))
        sys.exit(0 if same else 1)
    # Bytes, so Windows does not turn each line end into CRLF on the way to a file.
    sys.stdout.buffer.write(text.encode("utf-8"))
    print(f"fitted {len(samples)} touch changes, R^2 {r2:.4f}, weights x 2^16: {q}", file=sys.stderr)


if __name__ == "__main__":
    main()
