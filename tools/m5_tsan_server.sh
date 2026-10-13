#!/usr/bin/env bash
# M5 step 5: run the TSan build of lcache-server under redis-benchmark load and fail if
# ThreadSanitizer reports anything. Small capacity + random keys so eviction runs constantly;
# "guarded" also runs each shard's model under the shard locks.
#
# Usage: tools/m5_tsan_server.sh [port]   (after: cmake --build --preset tsan --target lcache-server)
# Needs redis-benchmark / redis-cli (apt install redis-tools). TSan needs ASLR off on this kernel
# (decisions D5), hence setarch -R.
set -euo pipefail
cd "$(dirname "$0")/.."

port=${1:-16390}
server=build/tsan/server/lcache-server
model=models/twitter26_1MiB_h10k_32x31.txt
logs=$(mktemp -d)
trap 'rm -rf "$logs"' EXIT
[[ -x $server ]] || { echo "build $server first (preset tsan)" >&2; exit 1; }

run() {
  local policy=$1; shift
  TSAN_OPTIONS="log_path=$logs/tsan halt_on_error=0" \
    setarch "$(uname -m)" -R "$server" --port "$port" --threads 4 --capacity 1MiB --shards 16 \
    --policy "$policy" "$@" >"$logs/$policy.out" 2>&1 &
  local pid=$!
  for _ in $(seq 50); do redis-cli -p "$port" PING >/dev/null 2>&1 && break; sleep 0.2; done
  echo "== $policy"
  redis-benchmark -p "$port" -c 50 --threads 4 -n 50000 -r 100000 -t ping,set,get -q
  redis-benchmark -p "$port" -c 50 --threads 4 -n 50000 -r 100000 -P 16 -t set,get -q
  kill -INT "$pid"
  wait "$pid"  # set -e: a non-zero exit fails the script
  cat "$logs/$policy.out"
}

run lru
run guarded --model "$model"

if compgen -G "$logs/tsan*" >/dev/null; then
  cat "$logs"/tsan*
  echo "FAIL: ThreadSanitizer reports above" >&2
  exit 1
fi
echo "OK: no ThreadSanitizer reports (throughput above is a TSan build, not a benchmark)"
