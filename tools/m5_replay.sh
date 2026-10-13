#!/usr/bin/env bash
# M5 step 6: hit rate through lcache-server vs the simulator, and the cost of sharding.
# For each policy and capacity, on the first N requests of the Twitter trace:
#   cachesim     the simulator with the trace's object ids
#   simulate     lcache-replay --mode simulate: the simulator core with the server's hashed ids
#   network S=s  lcache-replay against lcache-server --shards s (GET, SET on miss)
# Expected: network S=1 == simulate exactly (same ids, same rules); == cachesim exactly for
# policies that ignore id values (LRU). Deterministic: no time budget.
#
# Usage: tools/m5_replay.sh   (after: cmake --build --preset release)
set -euo pipefail
cd "$(dirname "$0")/.."

R=build/release
TRACE=data/twitter_cluster26_10M.oracleGeneral.bin
MODEL=models/twitter26_1MiB_h10k_32x31.txt
N=${N:-2000000}
PORT=${PORT:-16410}
OUT=bench/results/m5/replay.csv

model_args() { [[ $1 == guarded ]] && echo "--model $MODEL" || true; }

echo "mode,policy,capacity,shards,requests,hits,hit_rate,byte_hit_rate,too_large,unencodable,seconds" >"$OUT"
for cap in 1MiB 16MiB; do
  for policy in lru wtinylfu guarded; do
    # shellcheck disable=SC2046
    $R/apps/cachesim/cachesim --trace "$TRACE" -p "$policy" -c "$cap" -n "$N" $(model_args "$policy") |
      tail -1 | awk -F, -v OFS=, '{print "cachesim",$2,$3,1,$5,$6,$7,$10,"","",""}' >>"$OUT"
    # shellcheck disable=SC2046
    $R/apps/replay/lcache-replay "$TRACE" -n "$N" --mode simulate -c "$cap" --policy "$policy" \
      $(model_args "$policy") | tail -1 >>"$OUT"
    for shards in 1 8 64; do
      # shellcheck disable=SC2046
      $R/server/lcache-server --port "$PORT" --threads 2 --shards "$shards" --capacity "$cap" \
        --policy "$policy" $(model_args "$policy") >/dev/null &
      pid=$!
      for _ in $(seq 50); do redis-cli -p "$PORT" PING >/dev/null 2>&1 && break; sleep 0.1; done
      $R/apps/replay/lcache-replay "$TRACE" -n "$N" --port "$PORT" -c "$cap" --policy "$policy" \
        --shards "$shards" | tail -1 >>"$OUT"
      kill -INT "$pid"
      wait "$pid"
    done
    echo "done: $cap $policy" >&2
  done
done
column -s, -t "$OUT"
