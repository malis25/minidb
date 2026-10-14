# MiniDB Benchmarks

An HTTP-level benchmark harness for MiniDB. It starts the **real** HTTP API
in-process on an **ephemeral loopback port** (`bind_to_any_port`), so running a
benchmark never collides with a server already listening on `8080` or any other
port. It then drives two storage configurations through the same operations and
records throughput and latency percentiles.

## Configurations

| Name | Storage mode | Indexing |
|---|---|---|
| `resident` | Objects written to segments **and** kept in RAM | Hash equality index on every top-level field |
| `disk-backed` | Bodies read from segments on demand, 8 MiB LRU cache | B+ tree index on `email`; other fields are scanned |

The comparison is intentional: `query (indexed email)` is served from a B+ tree
in the disk configuration, while `query (username)` falls back to a stored-object
scan there (in the resident configuration it still uses the in-memory hash index).

## Operations

`create`, `read (by id)`, `query (indexed email)`, `query (username)`, `update`,
`batch x100`, `delete`.

## Build and run

```powershell
cmake -S . -B build
cmake --build build --config Debug
.\build\benchmarks\Debug\minidb_bench.exe
```

Options:

| Flag | Default | Meaning |
|---|---|---|
| `--objects N` | 2000 | Objects created per configuration |
| `--queries N` | 2000 | Read/query/update/delete iterations |
| `--batches N` | 20 | Number of batches executed |
| `--batch-size B` | 100 | Creates inside each batch |
| `--port P` | 0 | Fixed port to bind; `0` picks a free ephemeral port |
| `--out PATH` | `benchmarks/results/benchmark.json` | JSON output path |

Disable the harness entirely with `-DMINIDB_BUILD_BENCHMARKS=OFF`.

## Chart

```powershell
python benchmarks/plot_benchmark.py
```

Reads `benchmarks/results/benchmark.json` and writes
`benchmarks/results/benchmark.png`: throughput (log scale) and p95 latency (log
scale), grouped by operation and coloured by configuration. Requires
`matplotlib` and `numpy`.

## Notes

- Batch latency is reported as an **amortized per-operation** value (batch
  duration divided by `--batch-size`) so it is comparable with single-request
  operations.
- Generated `benchmarks/results/` output is ignored by Git.
- Percentiles use linear interpolation between the two nearest sorted samples.

## Write-path reservation sweep

`minidb_write_bench` is a second, core-only harness. It links `minidb::core`
directly (no HTTP) and measures how the next-ID **reservation block size**
affects write throughput. The manifest is only rewritten once per block, so a
larger block means fewer manifest writes and fewer IDs wasted after an unclean
stop than a smaller one. Run it in both Debug and Release:

```powershell
cmake --build build --config Debug --target minidb_write_bench
cmake --build build --config Release --target minidb_write_bench
.\build\benchmarks\Debug\minidb_write_bench.exe --out benchmarks/results/reservation-sweep.json
.\build\benchmarks\Release\minidb_write_bench.exe --objects 8000 --out benchmarks/results/reservation-sweep-release.json
python benchmarks/plot_reservation_sweep.py
```

| Flag | Default | Meaning |
|---|---|---|
| `--objects N` | 4000 | Objects stored per measured run |
| `--repeats R` | 3 | Runs per size; the fastest is reported |
| `--sizes a,b,c` | geometric 1…4096 | Reservation block sizes to test |
| `--out PATH` | `benchmarks/results/reservation-sweep.json` | JSON output path |

The plot script reads the Debug and Release JSON files and writes
`benchmarks/results/reservation-sweep.png` (throughput and manifest writes versus
block size). The chosen default is `default_next_id_reservation = 128`: measured
throughput reaches ~98% of its asymptote by 128 in both Debug and Release, while
larger blocks add only noise-level gains and skip more IDs after an unclean stop.

## Concurrent-client stress sweep

`minidb_concurrency_bench` is a third harness. Unlike `minidb_bench` (one
sequential client), it hammers the same in-process HTTP API from many client
threads at once and sweeps the number of concurrent clients, so it shows how
throughput and request latency behave as load rises past the machine's core
count. Each phase runs for a fixed wall-clock duration with every client issuing
requests as fast as it can:

```powershell
cmake --build build --config Debug --target minidb_concurrency_bench
.\build\benchmarks\Debug\minidb_concurrency_bench.exe `
    --objects 1000 --seconds 2 --peak-seconds 8 --levels 1,2,4,8,16,32,64,128 `
    --keep-alive --worker-pool 128 --out benchmarks/results/concurrency-1.json
python benchmarks/plot_concurrency.py
```

| Flag | Default | Meaning |
|---|---|---|
| `--objects N` | 2000 | Objects created per configuration before the sweep |
| `--seconds S` | 3 | Duration of each sweep phase in seconds |
| `--peak-seconds P` | 10 | Duration of the sustained mixed peak phase |
| `--levels a,b,c` | `1,2,4,8,16,32,64,128` | Concurrent client counts to sweep |
| `--keep-alive` | off | Reuse one connection per client instead of one per request |
| `--worker-pool N` | http-lib default | Server worker-pool size (`max(8, hardware-1)`) |
| `--port P` | 0 | Fixed port to bind; `0` picks a free ephemeral port |
| `--out PATH` | `benchmarks/results/concurrency.json` | JSON output path |

The three workloads are `read` (GET by id), `create` (POST) and `mixed`
(70% read, 15% indexed-email query, 10% update, 5% create). Objects created by a
`create`/`mixed` phase are deleted again so later phases start from the same
baseline. Because http-lib's default worker pool
(`max(8, hardware-1)`) caps how many keep-alive connections can be held at once,
`--worker-pool` enlarges it so the sweep measures the database layer rather than
that transport limit.

`plot_concurrency.py` reads one or more JSON files, keeps the best
(highest-throughput) run for each point, and writes
`benchmarks/results/concurrency-scaling.png`: a 2x3 grid of throughput (top) and
tail latency (bottom, p95 solid / p99 dashed) against concurrent clients, one
column per workload.
