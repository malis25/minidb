#!/usr/bin/env python3
"""Render MiniDB benchmark results (benchmarks/results/benchmark.json) as a chart.

The chart has two panels:
  * left  - throughput (operations/second) on a log scale
  * right - p95 latency (milliseconds) on a log scale

Bars are grouped by operation and coloured by storage configuration, so the
in-memory and disk-backed modes can be compared side by side.

Usage:
    python benchmarks/plot_benchmark.py [--input PATH] [--output PATH]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

CONFIG_COLORS = {
    "resident": "#4c72b0",
    "disk-backed": "#dd8452",
}
FALLBACK_COLORS = ["#55a868", "#c44e52", "#8172b3", "#937860"]


def load_samples(path: Path) -> list[dict]:
    with path.open(encoding="utf-8") as handle:
        data = json.load(handle)
    return data.get("samples", [])


def ordered_unique(values: list[str]) -> list[str]:
    seen: dict[str, None] = {}
    for value in values:
        seen.setdefault(value, None)
    return list(seen)


def color_for(config: str, index: int) -> str:
    return CONFIG_COLORS.get(config, FALLBACK_COLORS[index % len(FALLBACK_COLORS)])


def grouped_bars(axis, samples, configs, operations, metric, ylabel, title) -> None:
    """Draw grouped bars of `metric` per operation, one bar per configuration."""
    positions = np.arange(len(operations), dtype=float)
    width = 0.8 / max(len(configs), 1)
    for index, config in enumerate(configs):
        values = []
        for operation in operations:
            match = next(
                (s for s in samples if s["config"] == config and s["operation"] == operation),
                None,
            )
            values.append(max(match[metric], 1e-9) if match else 0.0)
        offset = positions - 0.4 + width * (index + 0.5)
        bars = axis.bar(offset, values, width=width * 0.92, label=config,
                        color=color_for(config, index), edgecolor="white", linewidth=0.4)
        axis.bar_label(bars, fmt="%.0f" if metric == "throughput" else "%.2f",
                       fontsize=7, padding=1)

    axis.set_yscale("log")
    axis.set_xticks(positions)
    axis.set_xticklabels(operations, rotation=20, ha="right", fontsize=8)
    axis.set_ylabel(ylabel)
    axis.set_title(title, fontsize=10, fontweight="bold")
    axis.grid(axis="y", which="both", linestyle=":", linewidth=0.5, alpha=0.6)
    axis.set_axisbelow(True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    default_input = Path(__file__).parent / "results" / "benchmark.json"
    default_output = Path(__file__).parent / "results" / "benchmark.png"
    parser.add_argument("--input", type=Path, default=default_input)
    parser.add_argument("--output", type=Path, default=default_output)
    args = parser.parse_args()

    if not args.input.exists():
        print(f"Benchmark results not found: {args.input}", file=sys.stderr)
        return 1

    samples = load_samples(args.input)
    if not samples:
        print("Benchmark results contain no samples.", file=sys.stderr)
        return 1

    configs = ordered_unique([s["config"] for s in samples])
    operations = ordered_unique([s["operation"] for s in samples])

    figure, (throughput_axis, latency_axis) = plt.subplots(1, 2, figsize=(15, 6))
    grouped_bars(throughput_axis, samples, configs, operations, "throughput",
                 "operations / second (log scale)", "Throughput")
    grouped_bars(latency_axis, samples, configs, operations, "p95_ms",
                 "p95 latency, ms (log scale)", "p95 latency")

    meta = json.loads(args.input.read_text(encoding="utf-8"))
    options = meta.get("options", {})
    subtitle = (f"objects={options.get('objects')}  "
                f"queries={options.get('queries')}  "
                f"batches={options.get('batches')}x{options.get('batch_size')}  "
                f"generated={meta.get('generated_at', '?')}")
    figure.suptitle("MiniDB API benchmark — resident vs disk-backed (B+ tree index)",
                    fontsize=13, fontweight="bold")
    figure.text(0.5, 0.94, subtitle, ha="center", fontsize=9, color="#444444")
    throughput_axis.legend(loc="upper right", fontsize=8, framealpha=0.9)
    figure.tight_layout(rect=(0, 0, 1, 0.92))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.output, dpi=150)
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
