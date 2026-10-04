#!/usr/bin/env python3
"""Print the arena tables that README.md and docs/ARENA.md quote.

    python bench/summarize_arena.py results/arena-windows

The engine's claims regenerate from results/ through summarize_results.py, and
until this existed the arena's did not: its numbers were read off the CSVs by
hand, which is exactly what CLAUDE.md forbids. Everything printed here is
computed from the committed CSVs, so a number in the documentation that this
script does not print is a number with no evidence behind it.

Reads the per-point summaries (<label>.csv) and the per-session rows
(<label>_sessions.csv) written by bench/run_arena_evidence.sh. Nothing here runs
a session or touches a binary.
"""
import csv
import os
import sys


def read(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def f(row, key, default=0.0):
    try:
        return float(row[key])
    except (KeyError, TypeError, ValueError):
        return default


def ns(value):
    """1000 -> '1 us'. The sweep's values are nanoseconds."""
    try:
        v = int(float(value))
    except (TypeError, ValueError):
        return str(value)
    if v < 1_000:
        return "%d ns" % v
    if v < 1_000_000:
        return "%g us" % (v / 1e3)
    return "%g ms" % (v / 1e6)


def rule(width=78):
    print("-" * width)


def health(rows, label):
    """The sanity check that comes before any result."""
    print("\n%s - market health (a book loose from the fundamental is not a market)" % label)
    print("  %-12s | %-18s | %-9s | %-10s | %s" % ("value", "|mid - fair| ticks", "worst", "one-sided", "trades"))
    rule(72)
    for r in rows:
        print("  %-12s | %18.2f | %6.0f tk | %9.1f%% | %8.0f"
              % (ns(r["value"]), f(r, "gap_ticks_median"), f(r, "gap_ticks_worst"),
                 f(r, "one_sided_pct"), f(r, "trades_median")))


def paired(rows, label, param):
    """The statistic the question is about: same seeds, differenced per seed."""
    if not rows:
        return
    seeds = int(f(rows[0], "seeds", 0))
    base = ns(rows[0]["value"])
    print("\n%s - maker value vs %s = %s, paired per seed, %d seeds" % (label, param, base, seeds))
    print("  %-12s | %-34s | %s" % ("value", "paired difference (median, range)", "separated from zero?"))
    rule(78)
    for r in rows:
        lo, hi = f(r, "maker_value_paired_lo"), f(r, "maker_value_paired_hi")
        first = r is rows[0]
        straddles = lo <= 0 <= hi
        print("  %-12s | %+10.2f (%+10.2f .. %+10.2f) | %s"
              % (ns(r["value"]), f(r, "maker_value_paired_median"), lo, hi,
                 "baseline" if first else ("no" if straddles else "yes")))


def decomposition(rows, label):
    """Where the money actually went: spread earned against adverse selection.

    Both are per-fill in the CSV, so the totals here are per-fill times the mean
    fill count, divided into currency units. This is the arithmetic behind the
    README's claim that the cost of being slow is liquidity provision.
    """
    print("\n%s - per-fill decomposition (edge = spread + adverse, currency units)" % label)
    print("  %-12s | %8s | %10s | %12s | %10s | %12s | %s"
          % ("value", "fills", "spread/fill", "spread total", "adv/fill", "adv total", "value"))
    rule(92)
    for r in rows:
        fills = f(r, "maker_fills_mean")
        sp, ad = f(r, "maker_spread_median"), f(r, "maker_adverse_median")
        print("  %-12s | %8.0f | %10.1f | %12.2f | %10.2f | %12.2f | %+10.2f"
              % (ns(r["value"]), fills, sp, fills * sp / 10000.0, ad, fills * ad / 10000.0,
                 f(r, "maker_value_median")))


def power(sessions, rows, label):
    """Standard error and minimum detectable effect for the paired difference.

    "Not separated from zero" must not be read as "nothing is there". With n
    paired seeds the paired standard error is sd/sqrt(n), and the smallest effect
    this many seeds could have resolved at roughly 95% confidence is about
    2.8 * that. Printed so a reader can tell a null from a sample too small to
    say — the distinction the sweep's own output cannot make.
    """
    if not sessions or not rows:
        return
    base_value = rows[0]["value"]
    by_value = {}
    for row in sessions:
        if row.get("name") != "maker":
            continue
        by_value.setdefault(row["value"], {})[row["seed"]] = f(row, "value_currency")
    base = by_value.get(base_value, {})
    if not base:
        return
    print("\n%s - power of the paired comparison" % label)
    print("  %-12s | %5s | %12s | %12s | %s"
          % ("value", "n", "paired mean", "std error", "min detectable effect (~95%)"))
    rule(80)
    for r in rows:
        point = by_value.get(r["value"], {})
        diffs = [point[s] - base[s] for s in point if s in base]
        n = len(diffs)
        if n < 2:
            continue
        mean = sum(diffs) / n
        var = sum((d - mean) ** 2 for d in diffs) / (n - 1)
        se = (var / n) ** 0.5
        print("  %-12s | %5d | %+12.2f | %12.2f | %+.2f"
              % (ns(r["value"]), n, mean, se, 2.8 * se))
    print("  A point whose paired mean is smaller than its minimum detectable effect is")
    print("  underpowered, not null. Both readings are honest; only one is a finding.")


def transfer(sessions, rows, label):
    """Who paid whom, and how that changed across the sweep.

    The closed-world identity says the values sum to zero, so a participant that
    gained did so at some named participant's expense. That turns the headline
    from an inference into a ledger: it shows whether a slow maker's money went
    to the informed trader (adverse selection) or to its competitor (competition
    for uninformed flow).

    Keyed on the participant INDEX, never the name: two bots are both called
    `noise`, so grouping by name silently averages them into one participant and
    the ledger stops summing to zero.
    """
    if not sessions or len(rows) < 2:
        return
    first, last = rows[0]["value"], rows[-1]["value"]
    names, totals, counts = {}, {}, {}
    for r in sessions:
        key = (r["value"], r["participant"])
        totals[key] = totals.get(key, 0.0) + f(r, "value_currency")
        counts[key] = counts.get(key, 0) + 1
        names[r["participant"]] = r.get("name", "?")

    def mean_at(value, who):
        key = (value, who)
        return totals[key] / counts[key] if counts.get(key) else None

    who_list = sorted({p for (v, p) in totals if v in (first, last)}, key=int)
    print("\n%s - who paid whom (mean value per participant, currency units)" % label)
    print("  %-3s %-11s | %14s | %14s | %s" % ("#", "participant", ns(first), ns(last), "transfer"))
    rule(72)
    sum_first = sum_last = 0.0
    for who in who_list:
        a, b = mean_at(first, who), mean_at(last, who)
        if a is None or b is None:
            continue
        sum_first += a
        sum_last += b
        print("  %-3s %-11s | %+14.2f | %+14.2f | %+14.2f" % (who, names[who], a, b, b - a))
    rule(72)
    print("  %-15s | %+14.2f | %+14.2f |" % ("sum", sum_first, sum_last))
    print("  The sums are the closed-world identity: every participant's gain is some other")
    print("  participant's loss, so a sum far from zero means this attribution is wrong.")


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    d = argv[1]
    if not os.path.isdir(d):
        print("not a directory: %s" % d, file=sys.stderr)
        return 2

    about = os.path.join(d, "about.txt")
    if os.path.exists(about):
        with open(about, encoding="utf-8") as fh:
            print(fh.read().rstrip())

    print("\nEvery number below is simulated. Reaction delays are inputs chosen by the")
    print("experimenter, not measurements; the engine's measured latency is elsewhere.")

    labels = sorted(
        os.path.splitext(os.path.basename(p))[0]
        for p in os.listdir(d)
        if p.endswith(".csv") and not p.endswith("_sessions.csv")
    )
    if not labels:
        print("\nno summary CSVs in %s - run bench/run_arena_evidence.sh first" % d, file=sys.stderr)
        return 1

    # The headline first, then its control, then everything else.
    order = [x for x in ("headline", "null", "no-rival") if x in labels]
    order += [x for x in labels if x not in order]

    for label in order:
        rows = read(os.path.join(d, label + ".csv"))
        if not rows:
            continue
        param = rows[0].get("param", "?")
        print("\n" + "=" * 78)
        print("%s  (sweeping %s)" % (label, param))
        print("=" * 78)
        health(rows, label)
        paired(rows, label, param)
        decomposition(rows, label)
        sessions = read(os.path.join(d, label + "_sessions.csv"))
        power(sessions, rows, label)
        transfer(sessions, rows, label)

    # The comparison the whole result turns on, if both runs are present.
    h = read(os.path.join(d, "headline.csv"))
    n = read(os.path.join(d, "null.csv"))
    if h and n:
        hd, nd = f(h[-1], "maker_value_paired_median"), f(n[-1], "maker_value_paired_median")
        print("\n" + "=" * 78)
        print("the control")
        print("=" * 78)
        print("  %s -> %s, maker value paired:" % (ns(h[0]["value"]), ns(h[-1]["value"])))
        print("    with an informed trader   %+10.2f" % hd)
        print("    nobody informed           %+10.2f" % nd)
        if hd != 0:
            print("    the null run retains %.0f%% of the effect, so it is not adverse selection."
                  % (100.0 * nd / hd))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
