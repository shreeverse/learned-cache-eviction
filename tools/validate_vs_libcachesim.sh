#!/usr/bin/env bash
# Cross-checks our policies against libCacheSim on the same trace and cache sizes (M1 and M2).
#
# Builds libCacheSim at a pinned commit, runs tools/libcachesim_exact.c (exact hit counts via its
# library API) and our cachesim for every policy, prints a comparison, and fails if a policy
# misses the check it is held to (see step 5). M1's LRU requirement is an exact match.
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

# 4. Run both, for every policy.
POLICIES=(lru lfu arc wtinylfu belady)
join() { local IFS=,; echo "$*"; }
: >"$OUT/libcachesim.csv"
for p in "${POLICIES[@]}"; do
  "$OUT/libcachesim_exact" "$TRACE" "$p" 0 "${SIZES_BYTES[@]}" | tail -n +2 >>"$OUT/libcachesim.csv"
  "$OUT/libcachesim_exact" "$TRACE" "$p" 1 "${SIZES_OBJECTS[@]}" | tail -n +2 >>"$OUT/libcachesim.csv"
done
build/release/apps/cachesim/cachesim -t "$TRACE" -p "$(join "${POLICIES[@]}")" \
  -c "$(join "${SIZES_BYTES[@]}")" >"$OUT/ours.csv"
build/release/apps/cachesim/cachesim -t "$TRACE" -p "$(join "${POLICIES[@]}")" \
  -c "$(join "${SIZES_OBJECTS[@]}")" --ignore-size | tail -n +2 >>"$OUT/ours.csv"

# 5. Compare. What each policy must satisfy (anything else is reported, not enforced):
#   lru, lfu  : exact match of every counter (same definitions as libCacheSim)
#   arc       : exact match with unit sizes; byte-size ARC has no canonical definition
#   belady    : ours >= libCacheSim with unit sizes (ours may bypass, which is the true optimum)
#   wtinylfu  : informational (count-min sketch vs libCacheSim's counting Bloom filter)
python3 - "$OUT/libcachesim.csv" "$OUT/ours.csv" "$LCS_COMMIT" <<'PY'
import csv, sys
cols = ["trace", "policy", "cache_size", "ignore_size", "requests", "hits", "bytes_requested", "bytes_hit"]
ref = {(r["policy"], r["cache_size"], r["ignore_size"]): r
       for r in (dict(zip(cols, row)) for row in csv.reader(open(sys.argv[1])))}
ours = {(r["policy"], r["cache_size"], r["ignore_size"]): r for r in csv.DictReader(open(sys.argv[2]))}

def exact(a, b, unit):
    same = a["requests"] == b["requests"] and a["hits"] == b["hits"]
    return same and (unit or (a["bytes_requested"] == b["bytes_requested"] and a["bytes_hit"] == b["bytes_hit"]))

print(f"libCacheSim commit {sys.argv[3][:7]}\n")
print("| policy | cache size | unit | libCacheSim hits | our hits | libCacheSim hit rate | our hit rate | diff (pp) | check |")
print("|---|---|---|---|---|---|---|---|---|")
failures = []
for key, a in ref.items():
    policy, size, ignore = key
    b, unit = ours[key], ignore == "true"
    ra, rb = int(a["hits"]) / int(a["requests"]), int(b["hits"]) / int(b["requests"])
    same = exact(a, b, unit)
    if policy in ("lru", "lfu") or (policy == "arc" and unit):
        check = "exact" if same else "FAIL (must be exact)"
    elif policy == "belady" and unit:
        check = "ours >= ref" if int(b["hits"]) >= int(a["hits"]) else "FAIL (must be >=)"
    else:
        check = "exact" if same else "info"
    if check.startswith("FAIL"):
        failures.append(key)
    print(f"| {policy} | {size} | {'objects' if unit else 'bytes'} | {a['hits']} | {b['hits']} "
          f"| {ra:.6f} | {rb:.6f} | {100 * (rb - ra):+.4f} | {check} |")
print(f"\nfailed checks: {len(failures)}")
sys.exit(1 if failures else 0)
PY
