#!/usr/bin/env bash
# M1 "done when": compares our LRU against libCacheSim's LRU on the same trace and cache sizes.
#
# Builds libCacheSim at a pinned commit, runs tools/libcachesim_exact.c (exact hit counts via its
# library API) and our cachesim, then prints a comparison and fails if any hit rate differs by
# more than 0.5 percentage points. For LRU we expect an exact match.
#
# Needs: git, cmake, ninja, gcc, pkg-config, glib-2.0 headers (Ubuntu: apt install libglib2.0-dev).
# If glib headers live somewhere non-standard, set PKG_CONFIG_PATH before running.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD
LCS_COMMIT=93529a1faef2c57bbdb9323213b78cad2adec1cd
LCS_DIR=${LCS_DIR:-$ROOT/build/libcachesim}
TRACE=data/cloudPhysicsIO.oracleGeneral.bin
SIZES_BYTES=(67108864 268435456 1073741824)   # 64 MiB, 256 MiB, 1 GiB
SIZES_OBJECTS=(100 1000 10000)                 # with --ignore-size
OUT=${OUT:-$ROOT/build/m1-validation}

pkg-config --exists glib-2.0 || {
  echo "glib-2.0 not found by pkg-config (apt install libglib2.0-dev, or set PKG_CONFIG_PATH)" >&2
  exit 1
}

# 1. libCacheSim at the pinned commit.
if [[ ! -d $LCS_DIR/.git ]]; then
  git clone -q https://github.com/1a1a11a/libCacheSim.git "$LCS_DIR"
fi
git -C "$LCS_DIR" fetch -q origin "$LCS_COMMIT" 2>/dev/null || true
git -C "$LCS_DIR" checkout -q "$LCS_COMMIT"
# zstd only matters for compressed traces; ours is uncompressed.
cmake -S "$LCS_DIR" -B "$LCS_DIR/_build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DOPT_SUPPORT_ZSTD_TRACE=OFF >/dev/null
cmake --build "$LCS_DIR/_build" --target libCacheSim >/dev/null

# 2. Exact-count harness linked against libCacheSim.
mkdir -p "$OUT"
gcc -O2 -I"$LCS_DIR/libCacheSim/include" -I"$LCS_DIR/libCacheSim/include/libCacheSim" \
  $(pkg-config --cflags glib-2.0) tools/libcachesim_exact.c "$LCS_DIR/_build/liblibCacheSim.a" \
  $(pkg-config --libs glib-2.0) -lstdc++ -lm -ldl -lpthread -o "$OUT/libcachesim_exact"

# 3. Our simulator.
cmake --preset release >/dev/null
cmake --build --preset release --target cachesim >/dev/null
tools/fetch_traces.sh >/dev/null

# 4. Run both.
join() { local IFS=,; echo "$*"; }
"$OUT/libcachesim_exact" "$TRACE" 0 "${SIZES_BYTES[@]}" >"$OUT/libcachesim.csv"
"$OUT/libcachesim_exact" "$TRACE" 1 "${SIZES_OBJECTS[@]}" | tail -n +2 >>"$OUT/libcachesim.csv"
build/release/apps/cachesim/cachesim -t "$TRACE" -c "$(join "${SIZES_BYTES[@]}")" >"$OUT/ours.csv"
build/release/apps/cachesim/cachesim -t "$TRACE" -c "$(join "${SIZES_OBJECTS[@]}")" --ignore-size \
  | tail -n +2 >>"$OUT/ours.csv"

# 5. Compare.
python3 - "$OUT/libcachesim.csv" "$OUT/ours.csv" "$LCS_COMMIT" <<'PY'
import csv, sys
ref = {(r["cache_size"], r["ignore_size"]): r for r in csv.DictReader(open(sys.argv[1]))}
ours = {(r["cache_size"], r["ignore_size"]): r for r in csv.DictReader(open(sys.argv[2]))}
print(f"libCacheSim commit {sys.argv[3][:7]}\n")
print("| cache size | unit | requests | libCacheSim hits | our hits | libCacheSim hit rate | our hit rate | diff (pp) |")
print("|---|---|---|---|---|---|---|---|")
worst, exact = 0.0, True
for key in ref:
    a, b = ref[key], ours[key]
    ra, rb = int(a["hits"]) / int(a["requests"]), int(b["hits"]) / int(b["requests"])
    diff = (rb - ra) * 100
    worst = max(worst, abs(diff))
    same = a["requests"] == b["requests"] and a["hits"] == b["hits"]
    if key[1] == "false":  # byte counts are only comparable when sizes are used
        same = same and a["bytes_requested"] == b["bytes_requested"] and a["bytes_hit"] == b["bytes_hit"]
    exact = exact and same
    unit = "objects" if key[1] == "true" else "bytes"
    print(f"| {key[0]} | {unit} | {b['requests']} | {a['hits']} | {b['hits']} | {ra:.6f} | {rb:.6f} | {diff:+.4f} |")
print(f"\nmax |diff| = {worst:.4f} pp (threshold 0.5 pp); exact match of all counters: {exact}")
sys.exit(0 if worst <= 0.5 else 1)
PY
