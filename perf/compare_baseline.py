#!/usr/bin/env python3
"""perf/compare_baseline.py -- Stage 5, Section 7: performance regression
harness. Compares a freshly-run benchmark CSV (see px/histogram.hpp's
append_csv, wired into bench_engine/bench_ring/bench_strategy via --csv=)
against the frozen baseline snapshot in perf/baseline/, reporting
baseline/current/%-change per metric and flagging anything past a tolerance
band for investigation.

Deliberately does NOT hard-fail on small noise (per the plan's own
instruction) -- exit code reflects only whether a FLAGGED regression exists,
and the tolerance is wide enough that this machine's normal run-to-run
laptop variance (see docs/PERFORMANCE.md's own discussion of that variance)
doesn't trip it on every run.

OPERATIONAL NOTE, observed directly while building this: running
`make perf-regression` immediately after a long battery of other benchmarks
(e.g. right after `bench-perf`'s dispatch-scaling sweep) can still flag a
tail-metric "regression" purely from thermal/Turbo-Boost state left over
from the prior runs, not from any code change -- seen firsthand, flagging a
different metric each time, with zero source changes between runs. Prefer
running this target on a stable/idle machine, or re-run it once before
trusting a flag; a flag on p999/p99.9 specifically, with p50/p90 clean, is
the signature of this rather than a real regression.

Usage:
  python perf/compare_baseline.py perf/baseline/bench_engine.csv perf/results/current_bench_engine.csv
"""
import csv
import sys

# Per-metric tolerance, calibrated against this machine's OWN measured
# zero-code-change noise, not guessed: two back-to-back runs of the
# unmodified bench_ring binary (no code change between them at all) swung
# same-thread push+pop's p999 by +65% purely from OS scheduling jitter on a
# laptop (no isolcpus, Turbo Boost, no tuned bare-metal box -- see
# docs/PERFORMANCE.md's "How to read this"). p999 is the 1-in-1000 sample out of
# 500,000 -- extremely sensitive to a single rare preemption -- so it gets a
# much wider band than p50/p90, which are dominated by real architectural
# cost rather than one-off scheduling noise. Widen these further if a real
# machine shows more baseline noise than this; the point is the tolerance
# should always trace back to an observed number, never an assumption.
TOLERANCE_PCT = {
    "p50_ns": 25.0,
    "p90_ns": 30.0,
    "p99_ns": 40.0,
    "p999_ns": 100.0,
}
METRICS = list(TOLERANCE_PCT.keys())  # p9999/max excluded -- both are single-sample-dominated and swing
                                        # wildly run to run even with zero code change, not usable as a gate


def load(path):
    with open(path, newline="") as f:
        return {row["label"]: row for row in csv.DictReader(f)}


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: compare_baseline.py <baseline.csv> <current.csv>")
    baseline = load(sys.argv[1])
    current = load(sys.argv[2])

    flagged = []
    print(f"{'label':<45} {'metric':<8} {'baseline':>10} {'current':>10} {'change':>8}")
    print("-" * 85)
    for label, base_row in baseline.items():
        if label not in current:
            print(f"{label:<45} -- missing from current run --")
            continue
        cur_row = current[label]
        for metric in METRICS:
            base_val = float(base_row[metric])
            cur_val = float(cur_row[metric])
            if base_val <= 0:
                continue
            pct_change = (cur_val - base_val) / base_val * 100.0
            tolerance = TOLERANCE_PCT[metric]
            marker = ""
            if pct_change > tolerance:
                marker = f"  <-- FLAGGED (regression, tolerance +{tolerance:.0f}%)"
                flagged.append((label, metric, base_val, cur_val, pct_change))
            print(f"{label:<45} {metric:<8} {base_val:>10.1f} {cur_val:>10.1f} {pct_change:>+7.1f}%{marker}")

    print()
    if flagged:
        print(f"{len(flagged)} metric(s) exceeded their tolerance -- investigate before "
              f"merging (not a hard CI failure by itself; see this file's header comment):")
        for label, metric, base_val, cur_val, pct_change in flagged:
            print(f"  {label} / {metric}: {base_val:.1f}ns -> {cur_val:.1f}ns ({pct_change:+.1f}%)")
        return 1
    print("no regressions past tolerance.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
