#!/usr/bin/env python3
"""Turns tools/m5_bench.sh raw redis-benchmark output into one CSV row per (config, test).

p99.9 is read from the "Latency by percentile distribution" table: the latency at the first
listed percentile >= 99.9% (HdrHistogram lists 99.902%, ...), so it is a slight upper bound.
"""
import csv
import re
import sys
from pathlib import Path

CONFIG = re.compile(r"# config: (\S+) clients=(\d+) pipeline=(\d+) datasize=(\d+)")
TEST = re.compile(r"====== (\w+) ======")
PCT = re.compile(r"([\d.]+)% <= ([\d.]+) milliseconds")
SUMMARY_COLS = ["avg_ms", "min_ms", "p50_ms", "p95_ms", "p99_ms", "max_ms"]


def parse(path):
    text = path.read_text().splitlines()
    m = CONFIG.match(text[0])
    if not m:
        raise ValueError(f"{path}: missing config line")
    name, clients, pipeline, datasize = m.groups()
    server = {}
    for line in text:
        if line.startswith("# server: "):
            for kv in re.findall(r"([a-z_]+)[=:](\d+)", line):
                server[kv[0]] = int(kv[1])
    rows, row, in_pct = [], None, False
    for i, line in enumerate(text):
        if t := TEST.match(line):
            row = {"config": name, "clients": clients, "pipeline": pipeline,
                   "datasize": datasize, "test": t.group(1)}
            rows.append(row)
        elif row is None:
            continue
        elif line.startswith("Latency by percentile distribution"):
            in_pct = True
        elif in_pct and (p := PCT.match(line)):
            if "p99_9_ms" not in row and float(p.group(1)) >= 99.9:
                row["p99_9_ms"] = p.group(2)
        elif in_pct and not line.strip():
            in_pct = False
        elif "throughput summary:" in line:
            row["rps"] = line.split(":")[1].split()[0]
        elif line.strip().startswith("avg") and "p99" in line:
            row.update(zip(SUMMARY_COLS, text[i + 1].split()))
    hits = server.get("hits", server.get("keyspace_hits"))
    misses = server.get("misses", server.get("keyspace_misses"))
    for r in rows:
        r["get_hit_rate"] = f"{hits / (hits + misses):.4f}" if hits is not None and hits + misses else ""
        r["evictions"] = server.get("evictions", server.get("evicted_keys", ""))
        for k in ("learned_evictions", "fallback_evictions", "hit_rate_trips", "latency_trips",
                  "budget_breaches"):
            r[k] = server.get(k, "")
    return rows


def main():
    raw, out = Path(sys.argv[1]), Path(sys.argv[2])
    rows = [r for f in sorted(raw.glob("*.txt")) for r in parse(f)]
    cols = ["config", "clients", "pipeline", "datasize", "test", "rps", "avg_ms", "p50_ms",
            "p95_ms", "p99_ms", "p99_9_ms", "max_ms", "get_hit_rate", "evictions",
            "learned_evictions", "fallback_evictions", "hit_rate_trips", "latency_trips",
            "budget_breaches"]
    with out.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    print(f"{len(rows)} rows -> {out}")


if __name__ == "__main__":
    main()
