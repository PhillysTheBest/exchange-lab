"""bench/summarize_results.py — turn results/<label>/ into the tables README.md and docs/ENGINE.md
quote. Reporting rule (fixed, not chosen after the fact): for each profile, each
percentile is the MEDIAN across the five seeds with the min-max range beside it;
"max" is the single worst order over every run; "over 2 us" is summed over every
run. Standard library only.

    python bench/summarize_results.py results/windows
"""
import csv
import statistics
import sys
from pathlib import Path


def rows(path):
    return list(csv.DictReader(open(path, newline=""))) if path.exists() else []


def fmt_range(values, digits=0):
    values = [float(v) for v in values]
    med = statistics.median(values)
    lo, hi = min(values), max(values)
    f = f",.{digits}f"
    return f"{med:{f}} ({lo:{f}}-{hi:{f}})" if len(values) > 1 else f"{med:{f}}"


def table(title, data, group_label):
    if not data:
        return
    print(f"\n{title}")
    print(f"| {group_label} | runs | p50 ns | p90 ns | p99 ns | p99.9 ns | p99.99 ns | worst order ns | orders over 2 us | M orders/s |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for profile in ("thin", "deep"):
        rs = [r for r in data if r["profile"] == profile]
        if not rs:
            continue
        over = sum(int(r["over_2us"]) for r in rs)
        total = sum(int(r["n"]) for r in rs)
        worst = max(float(r["max_ns"]) for r in rs)
        print(f"| {profile} | {len(rs)} | {fmt_range(r['p50_ns'] for r in rs)} | {fmt_range(r['p90_ns'] for r in rs)} "
              f"| {fmt_range(r['p99_ns'] for r in rs)} | {fmt_range(r['p999_ns'] for r in rs)} "
              f"| {fmt_range(r['p9999_ns'] for r in rs)} | {worst:,.0f} | {over:,} of {total:,} ({100 * over / total:.3f}%) "
              f"| {fmt_range((r['mops'] for r in rs), 2) if float(rs[0]['mops']) > 0.1 else 'paced'} |")
    stalls = sum(int(r["stalls"]) for r in data)
    allocs = sum(int(r["allocs"]) for r in data)
    print(f"\nring stalls: {stalls}, heap allocations in recorded windows: {allocs}")


def main():
    d = Path(sys.argv[1] if len(sys.argv) > 1 else "results/windows")
    print((d / "machine.txt").read_text().strip())
    seeds = rows(d / "seeds.csv")
    table("Unpaced, 5 seeds x 1M recorded orders (median across seeds, range in brackets):", seeds, "profile")
    for profile in ("thin", "deep"):
        rs = [r for r in seeds if r["profile"] == profile]
        if rs:
            print(f"{profile}: deepest side at end {max(int(r['deepest_side']) for r in rs)} levels, "
                  f"resting orders {max(int(r['resting']) for r in rs):,}")
    table("Replays, seed 42 x 5 (every run listed in the range):", rows(d / "repeat.csv"), "profile")
    table("Paced open loop at 50k orders/s, seed 42 x 3:", rows(d / "paced.csv"), "profile")
    for name in ("thin_repeat5", "deep_repeat5", "thin_paced50k", "deep_paced50k"):
        f = d / f"{name}.txt"
        if f.exists():
            for line in f.read_text().splitlines():
                if line.startswith(("replays", "best of K", "RESULT")):
                    print(f"{name}: {line.strip()}")


if __name__ == "__main__":
    main()
