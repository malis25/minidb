#!/usr/bin/env python3
"""Compare the pre/post optimization benchmark runs and render a PNG.

Reads the JSON emitted by minidb_bench (HTTP) and minidb_write_bench
(reservation sweep) from two result directories and plots throughput side by
side, plus a percent-change summary.

Usage:
  python benchmarks/plots/optimization_diff.py \
      --before benchmarks/results/opt-before \
      --after  benchmarks/results/opt-after \
      --out    benchmarks/results/optimization-before-after.png
"""

import argparse
import json
import os
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def load(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def http_map(document):
    """(config, operation) -> throughput."""
    return {(s["config"], s["operation"]): s["throughput"] for s in document["samples"]}


def write_map(document):
    """reservation -> throughput."""
    return {r["reservation"]: r["throughput"] for r in document["results"]}


def percent(before, after):
    return (after - before) / before * 100.0 if before else float("nan")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--before", required=True)
    parser.add_argument("--after", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    before_http = http_map(load(os.path.join(args.before, "http.json")))
    after_http = http_map(load(os.path.join(args.after, "http.json")))
    before_write = write_map(load(os.path.join(args.before, "write.json")))
    after_write = write_map(load(os.path.join(args.after, "write.json")))

    fig, (ax_http, ax_write) = plt.subplots(
        1, 2, figsize=(18, 8), gridspec_kw={"width_ratios": [1.35, 1.0]}
    )
    fig.suptitle(
        "MiniDB optimization: throughput before vs after", fontsize=16, fontweight="bold"
    )

    # --- HTTP panel -------------------------------------------------------
    keys = sorted(before_http, key=lambda k: (k[0], k[1]))
    labels = [f"{c}\n{o}" for c, o in keys]
    before_vals = [before_http[k] for k in keys]
    after_vals = [after_http[k] for k in keys]
    positions = range(len(keys))
    width = 0.4
    ax_http.bar([p - width / 2 for p in positions], before_vals, width, label="before",
                color="#9aa4b2")
    ax_http.bar([p + width / 2 for p in positions], after_vals, width, label="after",
                color="#2f7ed8")
    ax_http.set_ylabel("throughput (ops/sec)")
    ax_http.set_title("HTTP object operations")
    ax_http.set_xticks(list(positions))
    ax_http.set_xticklabels(labels, rotation=45, ha="right", fontsize=8)
    ax_http.legend()
    ax_http.grid(axis="y", alpha=0.3)
    for p, b, a in zip(positions, before_vals, after_vals):
        ax_http.annotate(f"{percent(b, a):+.0f}%", (p + width / 2, a),
                         textcoords="offset points", xytext=(0, 3), ha="center",
                         fontsize=7, color="#1b5e20")

    # --- write reservation panel -----------------------------------------
    reservations = sorted(before_write)
    xs = list(range(len(reservations)))
    ax_write.plot(xs, [before_write[r] for r in reservations], "o--",
                  label="before", color="#9aa4b2")
    ax_write.plot(xs, [after_write[r] for r in reservations], "o-",
                  label="after", color="#2f7ed8")
    ax_write.set_xticks(xs)
    ax_write.set_xticklabels([str(r) for r in reservations], rotation=45, fontsize=8)
    ax_write.set_xlabel("next-ID reservation block")
    ax_write.set_ylabel("throughput (ops/sec)")
    ax_write.set_title("Direct storage writes (manifest sweep)")
    ax_write.legend()
    ax_write.grid(alpha=0.3)
    for x, r in zip(xs, reservations):
        ax_write.annotate(f"{percent(before_write[r], after_write[r]):+.0f}%",
                          (x, after_write[r]), textcoords="offset points",
                          xytext=(0, 5), ha="center", fontsize=7, color="#1b5e20")

    fig.tight_layout(rect=(0, 0, 1, 0.96))
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=140)
    print(f"Wrote {out_path}")

    # --- textual diff -----------------------------------------------------
    print("\nHTTP throughput (ops/sec):")
    print(f"{'config':<12}{'operation':<22}{'before':>12}{'after':>12}{'change':>10}")
    for k in keys:
        print(f"{k[0]:<12}{k[1]:<22}{before_http[k]:>12.1f}{after_http[k]:>12.1f}"
              f"{percent(before_http[k], after_http[k]):>9.1f}%")
    print("\nWrite throughput (ops/sec):")
    print(f"{'reservation':<14}{'before':>12}{'after':>12}{'change':>10}")
    for r in reservations:
        print(f"{r:<14}{before_write[r]:>12.1f}{after_write[r]:>12.1f}"
              f"{percent(before_write[r], after_write[r]):>9.1f}%")


if __name__ == "__main__":
    main()
