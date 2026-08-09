#!/usr/bin/env python3
"""perf/plot_results.py -- Stage 5, Sections 4/6: turns
perf/results/throughput_sweep.csv and market_scaling_sweep.csv (produced by
perf/run_sweeps.py) into the two graphs the plan asks for:

  event rate -> tail latency   (perf/results/throughput_vs_latency.png)
  markets -> tail latency      (perf/results/scaling_vs_latency.png)

Plots only what's actually in the CSVs -- no invented data points, no
smoothing/interpolation between measured rows.
"""
import csv
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS_DIR = os.path.join(ROOT, "perf", "results")


def load_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def plot_throughput():
    path = os.path.join(RESULTS_DIR, "throughput_sweep.csv")
    rows = load_csv(path)
    achieved = [float(r["achieved_rate"]) for r in rows]
    p50 = [float(r["p50_ns"]) for r in rows]
    p99 = [float(r["p99_ns"]) for r in rows]
    p999 = [float(r["p999_ns"]) for r in rows]

    fig, ax = plt.subplots(figsize=(8, 5.5))
    ax.plot(achieved, p50, marker="o", label="p50")
    ax.plot(achieved, p99, marker="o", label="p99")
    ax.plot(achieved, p999, marker="o", label="p99.9")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("achieved event rate (events/sec, log scale)")
    ax.set_ylabel("event send -> Brain-applied latency (ns, log scale)")
    ax.set_title("Stage 5, Section 4: throughput vs. tail latency\n(10 markets, skewed, Normal profile)")
    ax.legend()
    ax.grid(True, which="both", alpha=0.3)
    fig.tight_layout()
    out = os.path.join(RESULTS_DIR, "throughput_vs_latency.png")
    fig.savefig(out, dpi=150)
    print(f"wrote {out}")


def plot_scaling():
    path = os.path.join(RESULTS_DIR, "market_scaling_sweep.csv")
    rows = load_csv(path)
    markets = [int(r["markets"]) for r in rows]
    p50 = [float(r["p50_ns"]) for r in rows]
    p99 = [float(r["p99_ns"]) for r in rows]
    rss_mb = [float(r["rss_end_bytes"]) / (1024 * 1024) for r in rows]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 5))
    ax1.plot(markets, p50, marker="o", label="p50")
    ax1.plot(markets, p99, marker="o", label="p99")
    ax1.set_xscale("log")
    ax1.set_yscale("log")
    ax1.set_xlabel("registered markets (log scale)")
    ax1.set_ylabel("latency (ns, log scale)")
    ax1.set_title("latency vs. market count\n(fixed rate=20,000/s, skewed)")
    ax1.legend()
    ax1.grid(True, which="both", alpha=0.3)

    ax2.plot(markets, rss_mb, marker="o", color="tab:green")
    ax2.set_xscale("log")
    ax2.set_xlabel("registered markets (log scale)")
    ax2.set_ylabel("RSS at end of run (MB)")
    ax2.set_title("memory vs. market count")
    ax2.grid(True, which="both", alpha=0.3)

    fig.suptitle("Stage 5, Section 6: multi-market scaling")
    fig.tight_layout()
    out = os.path.join(RESULTS_DIR, "scaling_vs_latency.png")
    fig.savefig(out, dpi=150)
    print(f"wrote {out}")


def main():
    plot_throughput()
    plot_scaling()


if __name__ == "__main__":
    main()
