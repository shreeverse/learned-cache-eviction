#!/usr/bin/env bash
# M5 step 7: throughput and latency of lcache-server vs redis-server, with redis-benchmark.
# Every configuration runs on a freshly started server, pinned with taskset: server on CPUs 0-7,
# client on 8-15 (WSL2 vCPUs; see docs/m5-results.md). Raw output goes to bench/results/m5/raw/,
# and tools/m5_parse_bench.py turns it into bench/results/m5/bench.csv.
#
# Matrix: -c {50,200} x -P {1,16} x -d {32,1024}, -t set,get -n 2000000 -r 1000000 --threads 4,
#   lcache-server: I/O threads {1,2,4,8} x policy {lru, wtinylfu, guarded}, 64 shards
#   redis-server 6.0.16: maxmemory + allkeys-lru, no persistence (single-threaded)
#   guarded at 8 threads with a time budget of 10 and 25 us (decisions D21)
# Both servers get 32 MiB, below the working set at both value sizes, so eviction runs throughout.
# lcache charges key + value bytes; Redis counts its own overhead too, so it holds fewer keys.
#
# Usage: tools/m5_bench.sh   (after: cmake --build --preset release; tools/m5_fetch_redis_benchmark.sh)
# ONLY=<regex> limits runs to config names matching it.
set -euo pipefail
cd "$(dirname "$0")/.."

SERVER=build/release/server/lcache-server
BENCH=build/tools/redis-benchmark-7
MODEL=models/twitter26_1MiB_h10k_32x31.txt
RAW=${RAW:-bench/results/m5/raw}
PORT=${PORT:-16500}
SERVER_CPUS=0-7
CLIENT_CPUS=8-15
N_REQ=${N_REQ:-2000000}
KEYSPACE=1000000
CAP=32MiB
mkdir -p "$RAW"

wait_ready() {
  for _ in $(seq 100); do redis-cli -p "$PORT" PING >/dev/null 2>&1 && return 0; sleep 0.1; done
  echo "server on $PORT did not start" >&2
  return 1
}

bench() {  # name clients pipeline datasize
  local out=$RAW/$1_c$2_P$3_d$4.txt
  echo "# config: $1 clients=$2 pipeline=$3 datasize=$4" >"$out"
  taskset -c "$CLIENT_CPUS" "$BENCH" -p "$PORT" -t set,get -n "$N_REQ" -r "$KEYSPACE" -c "$2" \
    -P "$3" -d "$4" --threads 4 2>&1 | tr '\r' '\n' | grep -v -E '^(SET|GET): rps=' >>"$out"
}

run_lcache() {  # name threads policy [extra server args...]
  local name=$1 threads=$2 policy=$3
  shift 3
  [[ -n ${ONLY:-} && ! $name =~ $ONLY ]] && return 0
  for c in 50 200; do for p in 1 16; do for d in 32 1024; do
    taskset -c "$SERVER_CPUS" "$SERVER" --port "$PORT" --threads "$threads" --shards 64 \
      --capacity "$CAP" --policy "$policy" "$@" >"$RAW/.server.out" &
    local pid=$!
    wait_ready
    bench "$name" "$c" "$p" "$d"
    kill -INT "$pid"
    wait "$pid"
    sed 's/^/# server: /' "$RAW/.server.out" >>"$RAW/${name}_c${c}_P${p}_d${d}.txt"
    echo "done: $name c=$c P=$p d=$d" >&2
  done; done; done
}

run_redis() {
  local name=redis
  [[ -n ${ONLY:-} && ! $name =~ $ONLY ]] && return 0
  for c in 50 200; do for p in 1 16; do for d in 32 1024; do
    taskset -c "$SERVER_CPUS" redis-server --port "$PORT" --save "" --appendonly no \
      --maxmemory 32mb --maxmemory-policy allkeys-lru >/dev/null &
    local pid=$!
    wait_ready
    bench "$name" "$c" "$p" "$d"
    redis-cli -p "$PORT" info stats | tr -d '\r' |
      grep -E '^(keyspace_hits|keyspace_misses|evicted_keys):' | sed 's/^/# server: /' \
      >>"$RAW/${name}_c${c}_P${p}_d${d}.txt"
    redis-cli -p "$PORT" shutdown nosave >/dev/null 2>&1 || true
    wait "$pid" || true
    echo "done: $name c=$c P=$p d=$d" >&2
  done; done; done
}

[[ -x $SERVER && -x $BENCH ]] || { echo "build $SERVER and $BENCH first" >&2; exit 1; }
for threads in 1 2 4 8; do
  run_lcache "lcache-lru-t$threads" "$threads" lru
  run_lcache "lcache-wtinylfu-t$threads" "$threads" wtinylfu
  run_lcache "lcache-guarded-t$threads" "$threads" guarded --model "$MODEL"
done
run_lcache lcache-guarded-budget10-t8 8 guarded --model "$MODEL" --budget-us 10
run_lcache lcache-guarded-budget25-t8 8 guarded --model "$MODEL" --budget-us 25
run_redis
rm -f "$RAW/.server.out"
python3 tools/m5_parse_bench.py "$RAW" "${CSV:-bench/results/m5/bench.csv}"
