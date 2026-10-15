#!/usr/bin/env python3
"""Visualise the durability trade-off for direct storage writes.

Compares two reservation sweeps produced by minidb_write_bench: one without the
durability barrier (stream flush only) and one that flushes every record to
stable storage (fsync / FlushFileBuffers). The chart shows absolute throughput
and the slowdown factor per reservation block.

Usage:
  python benchmarks/plots/durability_tradeoff.py \
      --fast benchmarks/results/opt-after/write.json \
      --durable benchmarks/results/opt-after/write-fsync.json \
      --out benchmarks/results/durability-tradeoff.png
"""

import argparse
import json
import os
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def load_results(path):
    with open(path, "r", encoding="utf-8") as handle:
        document = json.load(handle)
    return {r["reservation"]: r["throughput"] for r in document["results"]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fast", required=True)
    parser.add_argument("--durable", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    fast = load_results(args.fast)
    durable = load_results(args.durable)
    reservations = sorted(set(fast) & set(durable))
    xs = list(range(len(reservations)))

    fig, (ax_rate, ax_cost) = plt.subplots(
        1, 2, figsize=(18, 7.5), gridspec_kw={"width_ratios": [1.25, 1.0]}
    )
    fig.suptitle(
        "Durability trade-off: stream flush vs fsync per record",
        fontsize=16,
        fontweight="bold",
    )

    fast_vals = [fast[r] for r in reservations]
    durable_vals = [durable[r] for r in reservations]

    ax_rate.plot(xs, fast_vals, "o-", label="flush only (no fsync)", color="#2f7ed8")
    ax_rate.plot(xs, durable_vals, "o-", label="fsync per record (durable)", color="#d9534f")
    ax_rate.set_xticks(xs)
    ax_rate.set_xticklabels([str(r) for r in reservations], rotation=45, fontsize=8)
    ax_rate.set_xlabel("next-ID reservation block")
    ax_rate.set_ylabel("throughput (ops/sec)")
    ax_rate.set_title("Write throughput")
    ax_rate.legend()
    ax_rate.grid(alpha=0.3)
    for x, r in zip(xs, reservations):
        ax_rate.annotate(f"{fast[r]:.0f}", (x, fast[r]), textcoords="offset points",
                         xytext=(0, 6), ha="center", fontsize=7, color="#1b5e20")
        ax_rate.annotate(f"{durable[r]:.0f}", (x, durable[r]), textcoords="offset points",
                         xytext=(0, -12), ha="center", fontsize=7, color="#7f1d1d")

    slowdowns = [fast[r] / durable[r] for r in reservations]
    bars = ax_cost.bar(xs, slowdowns, color="#f0ad4e", edgecolor="#8a5a00")
    ax_cost.axhline(1.0, color="#555555", linestyle="--", linewidth=1)
    ax_cost.set_xticks(xs)
    ax_cost.set_xticklabels([str(r) for r in reservations], rotation=45, fontsize=8)
    ax_cost.set_xlabel("next-ID reservation block")
    ax_cost.set_ylabel("slowdown factor (x)")
    ax_cost.set_title("Cost of durability")
    ax_cost.grid(axis="y", alpha=0.3)
    for bar, value in zip(bars, slowdowns):
        ax_cost.annotate(f"{value:.1f}x", (bar.get_x() + bar.get_width() / 2, value),
                         textcoords="offset points", xytext=(0, 3), ha="center", fontsize=7)

    fig.tight_layout(rect=(0, 0, 1, 0.95))
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=140)
    print(f"Wrote {out_path}")

    print(f"\n{'reservation':<14}{'flush only':>14}{'fsync':>12}{'slowdown':>12}")
    for r in reservations:
        print(f"{r:<14}{fast[r]:>14.1f}{durable[r]:>12.1f}{fast[r] / durable[r]:>11.1f}x")


if __name__ == "__main__":
    main()
