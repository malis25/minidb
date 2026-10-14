#!/usr/bin/env python3
"""Render the next-ID reservation sweep as a chart.

Reads one or more sweep JSON files produced by `minidb_write_bench` (for example
the Debug and Release runs) and plots, on a log-2 x-axis:

  * left  - store throughput (operations/second)
  * right - manifest writes for the measured run

Usage:
    python benchmarks/plot_reservation_sweep.py [--inputs PATH [PATH ...]] [--output PATH]
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

SERIES_COLORS = ["#4c72b0", "#dd8452", "#55a868", "#c44e52"]


def load(path: Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    here = Path(__file__).parent / "results"
    parser.add_argument(
        "--inputs",
        type=Path,
        nargs="+",
        default=[here / "reservation-sweep.json", here / "reservation-sweep-release.json"],
    )
    parser.add_argument("--output", type=Path, default=here / "reservation-sweep.png")
    args = parser.parse_args()

    available = [path for path in args.inputs if path.exists()]
    if not available:
        raise SystemExit(f"No sweep results found: {args.inputs}")

    figure, (throughput_axis, writes_axis) = plt.subplots(1, 2, figsize=(14, 5.5))
    default_reservation = None

    for index, path in enumerate(available):
        data = load(path)
        results = sorted(data["results"], key=lambda row: row["reservation"])
        sizes = [row["reservation"] for row in results]
        throughput = [row["throughput"] for row in results]
        writes = [row["manifest_writes"] for row in results]
        label = path.stem.replace("reservation-sweep", "sweep").strip("-") or "sweep"
        label = {"sweep": "Debug", "sweep-release": "Release"}.get(label, label)
        default_reservation = data.get("default_reservation", default_reservation)

        color = SERIES_COLORS[index % len(SERIES_COLORS)]
        throughput_axis.plot(sizes, throughput, marker="o", markersize=4, linewidth=1.6,
                             color=color, label=label)
        writes_axis.plot(sizes, writes, marker="o", markersize=4, linewidth=1.6,
                         color=color, label=label)

    for axis in (throughput_axis, writes_axis):
        axis.set_xscale("log", base=2)
        axis.grid(True, which="both", linestyle=":", linewidth=0.5, alpha=0.6)
        axis.set_axisbelow(True)
        axis.set_xlabel("next-ID reservation block size")
        if default_reservation:
            axis.axvline(default_reservation, color="#333333", linestyle="--", linewidth=1.0,
                         alpha=0.8)
            axis.annotate(f"chosen default = {default_reservation}",
                          xy=(default_reservation, axis.get_ylim()[0]),
                          xytext=(4, 4), textcoords="offset points", fontsize=8, color="#333333")

    throughput_axis.set_ylabel("store throughput (operations / second)")
    throughput_axis.set_title("Write throughput vs reservation block", fontsize=10,
                              fontweight="bold")
    throughput_axis.legend(fontsize=8, loc="lower right")

    writes_axis.set_yscale("log")
    writes_axis.set_ylabel("manifest writes (log scale)")
    writes_axis.set_title("Manifest writes vs reservation block", fontsize=10,
                          fontweight="bold")
    writes_axis.legend(fontsize=8, loc="upper right")

    figure.suptitle("MiniDB next-ID reservation sweep — manifest writes vs throughput",
                    fontsize=13, fontweight="bold")
    figure.tight_layout(rect=(0, 0, 1, 0.94))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output, dpi=150)
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
