#!/usr/bin/env python3
"""Compare the HTTP benchmark before and after the next-ID reservation change.

Reads one or more `minidb_bench` JSON files for each side (the "before" files
were produced with `default_next_id_reservation = 1`, i.e. a manifest write on
every create; the "after" files with `= 128`). For every configuration and
operation it keeps the best (highest-throughput) run on each side so the
comparison is not skewed by background load, then draws a grouped bar chart with
one panel per storage configuration.

Usage:
    python benchmarks/plot_comparison.py [--before GLOB ...] [--after GLOB ...]
                                         [--output PATH]
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

OPERATIONS = [
    "create",
    "read (by id)",
    "query (indexed email)",
    "query (username)",
    "update",
    "batch x100",
    "delete",
]
SHORT = {
    "create": "create",
    "read (by id)": "read",
    "query (indexed email)": "query\n(indexed)",
    "query (username)": "query\n(scan)",
    "update": "update",
    "batch x100": "batch\nx100",
    "delete": "delete",
}
CONFIGS = ["resident", "disk-backed"]
BEFORE_COLOR = "#c44e52"
AFTER_COLOR = "#4c72b0"


def load_best(patterns: list[str]) -> dict[tuple[str, str], float]:
    """Map (config, operation) -> best throughput across all matched files."""
    best: dict[tuple[str, str], float] = {}
    files: list[str] = []
    for pattern in patterns:
        files.extend(sorted(glob.glob(pattern)))
    files = sorted(set(files))
    if not files:
        raise SystemExit(f"No benchmark files matched: {patterns}")
    for path in files:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
        for sample in data["samples"]:
            if sample["failures"]:
                continue
            key = (sample["config"], sample["operation"])
            best[key] = max(best.get(key, 0.0), sample["throughput"])
    return best


def main() -> int:
    here = Path(__file__).parent / "results"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", nargs="+", default=[str(here / "benchmark-before-*.json")])
    parser.add_argument("--after", nargs="+", default=[str(here / "benchmark-after-*.json")])
    parser.add_argument("--output", type=Path, default=here / "benchmark-comparison.png")
    args = parser.parse_args()

    before = load_best(args.before)
    after = load_best(args.after)

    figure, axes = plt.subplots(1, 2, figsize=(15, 6))
    positions = np.arange(len(OPERATIONS))
    width = 0.38

    for axis, config in zip(axes, CONFIGS):
        before_values = [before.get((config, op), 0.0) for op in OPERATIONS]
        after_values = [after.get((config, op), 0.0) for op in OPERATIONS]

        before_bars = axis.bar(positions - width / 2, before_values, width,
                               label="before (manifest write per create)", color=BEFORE_COLOR)
        after_bars = axis.bar(positions + width / 2, after_values, width,
                              label="after (IDs reserved in blocks)", color=AFTER_COLOR)

        for before_value, after_value, x in zip(before_values, after_values, positions):
            if before_value <= 0:
                continue
            change = (after_value - before_value) / before_value * 100.0
            top = max(before_value, after_value)
            axis.annotate(f"{change:+.0f}%", xy=(x + width / 2, top),
                          xytext=(0, 3), textcoords="offset points", ha="center",
                          fontsize=8, fontweight="bold",
                          color=AFTER_COLOR if change >= 0 else BEFORE_COLOR)

        axis.set_yscale("log")
        axis.set_ylim(bottom=min(v for v in before_values + after_values if v > 0) * 0.5)
        axis.set_xticks(positions)
        axis.set_xticklabels([SHORT[op] for op in OPERATIONS], fontsize=9)
        axis.set_ylabel("throughput (operations / second, log scale)")
        axis.set_title(config, fontsize=11, fontweight="bold")
        axis.grid(True, axis="y", which="both", linestyle=":", linewidth=0.5, alpha=0.6)
        axis.set_axisbelow(True)

    axes[0].legend(fontsize=8, loc="upper right")
    figure.suptitle("MiniDB HTTP benchmark — before vs after next-ID reservation",
                    fontsize=14, fontweight="bold")
    figure.tight_layout(rect=(0, 0, 1, 0.95))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output, dpi=150)
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
