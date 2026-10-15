#!/usr/bin/env python3
"""Overall view of the final MiniDB storage state.

Combines three sources produced by the Release benchmarks:

  * a write reservation sweep without the durability barrier,
  * the same sweep with fsync (FlushFileBuffers) on every record,
  * the HTTP operation benchmark for resident and disk-backed databases.

The result summarises throughput in each mode and, below it, the durability and
recovery guarantees that the storage now provides.

Usage:
  python benchmarks/plots/final_state_overview.py \
      --nodurable benchmarks/results/final/write-nodurable.json \
      --durable benchmarks/results/final/write-durable.json \
      --http benchmarks/results/final/http.json \
      --out benchmarks/results/final-state-overview.png
"""

import argparse
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch


def load_write(path):
    with open(path, "r", encoding="utf-8") as handle:
        document = json.load(handle)
    return {r["reservation"]: r["throughput"] for r in document["results"]}


def load_http(path):
    with open(path, "r", encoding="utf-8") as handle:
        document = json.load(handle)
    table = {}
    for sample in document["samples"]:
        table[(sample["config"], sample["operation"])] = sample["throughput"]
    return table


def build_guarantee_rows():
    return [
        ("Power loss / kernel panic", "fsync before ack (FlushFileBuffers / fsync)", "Protected"),
        ("Process kill in mid-write", "ordered writes, atomic manifest replace", "Protected"),
        ("Torn trailing record", "truncated to record start, earlier records kept", "Recovered"),
        ("Torn record in sealed segment", "startup fails instead of dropping data", "Reported"),
        ("Corrupt JSON payload", "explicit parse error naming the segment", "Reported"),
        ("In-place bit flip", "per-record FNV-1a checksum detects it", "Reported"),
        ("Stale manifest reservation", "scan + max, IDs are never reused", "Protected"),
        ("Torn manifest write", "temporary file + atomic replace", "Protected"),
        ("Missing / corrupt index file", "rebuilt from the segment log", "Recovered"),
        ("Corrupt B+ tree header", "header checksum triggers a rebuild", "Recovered"),
    ]


STATUS_COLORS = {
    "Protected": "#2e7d32",
    "Recovered": "#1565c0",
    "Reported": "#ef6c00",
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nodurable", required=True)
    parser.add_argument("--durable", required=True)
    parser.add_argument("--http", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    fast = load_write(args.nodurable)
    durable = load_write(args.durable)
    reservations = sorted(set(fast) & set(durable))
    xs = list(range(len(reservations)))

    http = load_http(args.http)
    operations = ["create", "read (by id)", "update", "delete",
                  "query (username)", "query (indexed email)", "batch x100"]

    fig = plt.figure(figsize=(19, 12.5))
    grid = fig.add_gridspec(2, 2, height_ratios=[1.0, 0.95], hspace=0.28, wspace=0.18)

    # --- Write throughput ---------------------------------------------------
    ax_write = fig.add_subplot(grid[0, 0])
    ax_write.plot(xs, [fast[r] for r in reservations], "o-", color="#2f7ed8",
                  label="flush only (process-crash safe)")
    ax_write.plot(xs, [durable[r] for r in reservations], "o-", color="#d9534f",
                  label="fsync per record (durable)")
    ax_write.set_xticks(xs)
    ax_write.set_xticklabels([str(r) for r in reservations], rotation=45, fontsize=8)
    ax_write.set_xlabel("next-ID reservation block")
    ax_write.set_ylabel("throughput (ops/sec)")
    ax_write.set_title("Direct storage writes: durability cost", fontsize=13, fontweight="bold")
    ax_write.legend()
    ax_write.grid(alpha=0.3)
    peak = reservations[-1]
    ax_write.annotate(
        f"{fast[peak] / durable[peak]:.1f}x slower at reservation {peak}",
        xy=(xs[-1], durable[peak]), xytext=(-150, -55), textcoords="offset points",
        fontsize=10, color="#7f1d1d",
        arrowprops={"arrowstyle": "->", "color": "#7f1d1d"},
    )

    # --- HTTP throughput ----------------------------------------------------
    ax_http = fig.add_subplot(grid[0, 1])
    positions = list(range(len(operations)))
    height = 0.38
    resident = [http.get(("resident", op), 0.0) for op in operations]
    disk = [http.get(("disk-backed", op), 0.0) for op in operations]
    ax_http.barh([p + height / 2 for p in positions], resident, height,
                 color="#5b9bd5", label="resident")
    ax_http.barh([p - height / 2 for p in positions], disk, height,
                 color="#70ad47", label="disk-backed")
    ax_http.set_yticks(positions)
    ax_http.set_yticklabels(operations, fontsize=9)
    ax_http.set_xscale("log")
    ax_http.set_xlabel("throughput (ops/sec, log scale)")
    ax_http.set_title("HTTP operations: final build (fsync creates)", fontsize=13,
                      fontweight="bold")
    ax_http.legend(loc="lower right")
    ax_http.grid(axis="x", alpha=0.3)
    for p, value in zip(positions, resident):
        ax_http.annotate(f"{value:,.0f}", (value, p + height / 2), fontsize=7,
                         xytext=(3, 0), textcoords="offset points", va="center")
    for p, value in zip(positions, disk):
        ax_http.annotate(f"{value:,.0f}", (value, p - height / 2), fontsize=7,
                         xytext=(3, 0), textcoords="offset points", va="center")

    # --- Guarantee table ----------------------------------------------------
    ax_table = fig.add_subplot(grid[1, :])
    ax_table.axis("off")
    rows = build_guarantee_rows()
    cells = [[scenario, behavior, status] for scenario, behavior, status in rows]
    table = ax_table.table(
        cellText=cells,
        colLabels=["Failure / crash scenario", "Recovery behaviour", "Outcome"],
        colWidths=[0.30, 0.52, 0.14],
        loc="center",
        cellLoc="left",
    )
    table.auto_set_font_size(False)
    table.set_fontsize(10.5)
    table.scale(1, 1.75)
    for (row, col), cell in table.get_celld().items():
        cell.set_edgecolor("#bdbdbd")
        if row == 0:
            cell.set_facecolor("#37474f")
            cell.set_text_props(color="white", fontweight="bold")
            continue
        status = rows[row - 1][2]
        if col == 2:
            cell.set_facecolor(STATUS_COLORS[status])
            cell.set_text_props(color="white", fontweight="bold", ha="center")
        elif row % 2 == 0:
            cell.set_facecolor("#f5f5f5")
    ax_table.set_title("Durability and recovery guarantees (95 unit tests, 1 skipped: symlink)",
                       fontsize=13, fontweight="bold", pad=14)

    fig.legend(
        handles=[Patch(facecolor=STATUS_COLORS["Protected"], label="durable / structural guarantee"),
                 Patch(facecolor=STATUS_COLORS["Recovered"], label="automatically recovered"),
                 Patch(facecolor=STATUS_COLORS["Reported"], label="explicit, loud error")],
        loc="lower center", ncol=3, frameon=False, bbox_to_anchor=(0.5, 0.005),
    )

    fig.suptitle("MiniDB storage: final state overview", fontsize=17, fontweight="bold")
    fig.tight_layout(rect=(0, 0.035, 1, 0.97))

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=140)
    print(f"Wrote {out_path}")


if __name__ == "__main__":
    main()
