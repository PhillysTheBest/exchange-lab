#!/usr/bin/env python3
"""perf/run_sweeps.py -- Stage 5, Sections 4/5/6: drives
build/release/bench_workload_throughput.exe across the rate/market-count
sweep points the plan asks for, one fresh process per point (so a stuck or
crashed point can't corrupt the next one's measurement), appending one CSV
row per run.

Usage:
  python perf/run_sweeps.py throughput   # Section 4: rate sweep, fixed markets
  python perf/run_sweeps.py backpressure # Section 5: one deliberately-overloaded run
  python perf/run_sweeps.py scaling      # Section 6: market-count sweep, fixed rate
  python perf/run_sweeps.py all          # all three, in order

Does not invent numbers or interpret results -- it only runs the already-
built binary and collects its own CSV output (see
bench/bench_workload_throughput.cpp for what's measured and how). Requires
`mingw32-make bench-perf` to have been run first.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "release", "bench_workload_throughput.exe")
RESULTS_DIR = os.path.join(ROOT, "perf", "results")


def run_one(csv_path, label, rate, duration_sec, markets, profile="normal", market_dist="skewed", seed=42):
    args = [
        EXE,
        f"--rate={rate}",
        f"--duration-sec={duration_sec}",
        f"--markets={markets}",
        f"--profile={profile}",
        f"--market-dist={market_dist}",
        f"--seed={seed}",
        f"--csv={csv_path}",
        f"--label={label}",
    ]
    print(f"-- {label}: rate={rate}/s markets={markets} profile={profile} dist={market_dist} "
          f"duration={duration_sec}s", flush=True)
    result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=duration_sec + 30)
    print(result.stdout)
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        raise SystemExit(f"run failed: {label} (exit {result.returncode})")


def sweep_throughput():
    """Section 4: event rate -> tail latency, fixed market count.

    Starting points are the plan's own suggested rates; the tool reports
    ACHIEVED rate alongside target, so the CSV shows exactly where target
    and achieved diverge (the saturation knee) rather than assuming any of
    these exact numbers are meaningful ceilings on this machine.
    """
    csv_path = os.path.join(RESULTS_DIR, "throughput_sweep.csv")
    if os.path.exists(csv_path):
        os.remove(csv_path)
    rates = [1_000, 10_000, 50_000, 100_000, 250_000, 500_000, 1_000_000]
    for rate in rates:
        run_one(csv_path, "throughput_sweep", rate, duration_sec=3, markets=10)
    print(f"\nwrote {csv_path}")


def run_backpressure():
    """Section 5: one deliberately-unreachable target rate, to force the ring
    into sustained near-full occupancy and observe the existing spin-wait
    policy's behavior under it (see bench_workload_throughput.cpp's ring
    occupancy sampling and the produced CSV's ring_occ_max/mean columns).
    """
    csv_path = os.path.join(RESULTS_DIR, "backpressure_run.csv")
    if os.path.exists(csv_path):
        os.remove(csv_path)
    run_one(csv_path, "backpressure_overload", rate=5_000_000, duration_sec=5, markets=1)
    print(f"\nwrote {csv_path}")


def sweep_scaling():
    """Section 6: market count -> tail latency/RSS, fixed rate, skewed
    traffic distribution, one PassiveMarketMaker per market.
    """
    csv_path = os.path.join(RESULTS_DIR, "market_scaling_sweep.csv")
    if os.path.exists(csv_path):
        os.remove(csv_path)
    market_counts = [1, 10, 50, 100, 500, 1000]
    for n in market_counts:
        run_one(csv_path, "market_scaling_sweep", rate=20_000, duration_sec=3, markets=n, market_dist="skewed")
    print(f"\nwrote {csv_path}")


def main():
    os.makedirs(RESULTS_DIR, exist_ok=True)
    if not os.path.exists(EXE):
        raise SystemExit(f"{EXE} not found -- run `mingw32-make bench-perf` first")

    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    if which in ("throughput", "all"):
        sweep_throughput()
    if which in ("backpressure", "all"):
        run_backpressure()
    if which in ("scaling", "all"):
        sweep_scaling()
    if which not in ("throughput", "backpressure", "scaling", "all"):
        raise SystemExit(f"unknown sweep: {which}")


if __name__ == "__main__":
    main()
