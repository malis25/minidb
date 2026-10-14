#!/usr/bin/env python3
"""Plot the concurrent-client stress benchmark.

Reads one or more `minidb_concurrency_bench` JSON files and, for every
(configuration, workload, client count) point, keeps the best (highest
throughput) run so the curves are not skewed by background load. It then draws a
2x3 grid:

  * top row    - throughput (operations / second) against concurrent clients
  * bottom row - tail latency (p95 solid, p99 dashed) against concurrent clients

The columns are the three workloads (read, create, mixed) and every panel
overlays the two storage configurations. The sustained mixed phase is marked
with a star on the throughput panels.

Usage:
    python benchmarks/plot_concurrency.py [--input GLOB ...] [--output PATH]
"""

from __future__ import annotations

import argparse
import glob
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

WORKLOADS = ["read", "create", "mixed"]
CONFIGS = ["resident", "disk-backed"]
COLORS = {"resident": "#4c72b0", "disk-backed": "#dd8452"}
LABELS = {"resident": "resident", "disk-backed": "disk-backed (8 MiB cache)"}
TITLES = {"read": "read (GET by id)", "create": "create (POST)", "mixed": "mixed (70/15/10/5)"}


def load_best(patterns: list[str]) -> tuple[dict, list[str]]:
    """Map (config, workload, concurrency, sustained) -> best sample."""
    files: list[str] = []
    for pattern in patterns:
        files.extend(sorted(glob.glob(pattern)))
    files = sorted(set(files))
    if not files:
        raise SystemExit(f"No benchmark files matched: {patterns}")

    best: dict[tuple[str, str, int, bool], dict] = {}
    for path in files:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
        for sample in data["samples"]:
            key = (
                sample["config"],
                sample["workload"],
                int(sample["concurrency"]),
                bool(sample["sustained"]),
            )
            if key not in best or sample["throughput"] > best[key]["throughput"]:
                best[key] = sample
    return best, files


def sweep(best: dict, config: str, workload: str) -> list[tuple[int, dict]]:
    points = [
        (key[2], sample)
        for key, sample in best.items()
        if key[0] == config and key[1] == workload and not key[3]
    ]
    return sorted(points, key=lambda item: item[0])


def sustained(best: dict, config: str, workload: str) -> dict | None:
    for key, sample in best.items():
        if key[0] == config and key[1] == workload and key[3]:
            return sample
    return None


def main() -> int:
    here = Path(__file__).parent / "results"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", nargs="+", default=[str(here / "concurrency-[0-9].json")])
    parser.add_argument("--output", type=Path, default=here / "concurrency-scaling.png")
    args = parser.parse_args()

    best, files = load_best(args.input)
    print(f"Read {len(files)} file(s):")
    for path in files:
        print(f"  {path}")

    codes = sorted({key[2] for key in best})
    xticks = sorted({1, 2, 4, 8, 16, 32, 64, 128} | set(codes))

    figure, axes = plt.subplots(2, 3, figsize=(18, 10))

    for column, workload in enumerate(WORKLOADS):
        top = axes[0][column]
        bottom = axes[1][column]

        for config in CONFIGS:
            points = sweep(best, config, workload)
            if not points:
                continue
            xs = [point[0] for point in points]
            throughput = [point[1]["throughput"] for point in points]
            top.plot(xs, throughput, marker="o", markersize=4, linewidth=1.8,
                     color=COLORS[config], label=LABELS[config])

            p95 = [point[1]["p95_ms"] for point in points]
            p99 = [point[1]["p99_ms"] for point in points]
            bottom.plot(xs, p95, marker="o", markersize=4, linewidth=1.8,
                        color=COLORS[config], linestyle="-",
                        label=f"{config} p95")
            bottom.plot(xs, p99, marker="^", markersize=4, linewidth=1.4,
                        color=COLORS[config], linestyle="--",
                        label=f"{config} p99")

            peak = sustained(best, config, workload)
            if peak is not None:
                top.plot([peak["concurrency"]], [peak["throughput"]], marker="*",
                         markersize=16, color=COLORS[config], zorder=5)
                if peak["failures"]:
                    top.annotate(f"{peak['failures']} failed",
                                 xy=(peak["concurrency"], peak["throughput"]),
                                 xytext=(-6, 10), textcoords="offset points",
                                 ha="right", fontsize=8, color=COLORS[config])

        top.set_xscale("log", base=2)
        top.set_xticks(xticks)
        top.set_xticklabels([str(tick) for tick in xticks])
        top.set_title(TITLES[workload], fontsize=12, fontweight="bold")
        top.set_xlabel("concurrent clients")
        top.set_ylabel("throughput (operations / second)")
        top.grid(True, which="both", linestyle=":", linewidth=0.5, alpha=0.6)
        top.set_axisbelow(True)

        bottom.set_xscale("log", base=2)
        bottom.set_yscale("log")
        bottom.set_xticks(xticks)
        bottom.set_xticklabels([str(tick) for tick in xticks])
        bottom.set_xlabel("concurrent clients")
        bottom.set_ylabel("request latency (ms, log scale)")
        bottom.grid(True, which="both", linestyle=":", linewidth=0.5, alpha=0.6)
        bottom.set_axisbelow(True)

    handles, labels = axes[0][0].get_legend_handles_labels()
    axes[0][0].legend(handles, labels, fontsize=8, loc="lower left")
    handles, labels = axes[1][0].get_legend_handles_labels()
    axes[1][0].legend(handles, labels, fontsize=7, loc="upper left", ncol=2)

    figure.suptitle("MiniDB concurrent-client stress benchmark — throughput and tail "
                    "latency vs concurrent clients",
                    fontsize=15, fontweight="bold")
    figure.tight_layout(rect=(0, 0, 1, 0.96))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output, dpi=150)
    print(f"\nWrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
