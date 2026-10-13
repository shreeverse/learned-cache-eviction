#!/usr/bin/env bash
# M5 finding: W-TinyLFU's hit rate depends on how keys are numbered. Writes the first 2M Twitter
# requests with object ids relabeled (identity, lcache-server's key hash, a multiplicative hash,
# 9 random permutations), then runs cachesim on each. Only sketch collisions depend on ids, so a
# well-behaved policy should be nearly invariant; LRU is exactly invariant (control).
# Output: bench/results/m5/wtinylfu_labeling.csv. Needs the release cachesim.
set -euo pipefail
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
OUT=bench/results/m5/wtinylfu_labeling.csv

python3 -I - "$TMP" <<'PY'
import random, struct, sys
out, n, mask = sys.argv[1], 2_000_000, (1 << 64) - 1
rec = struct.Struct('<IQIq')
data = open('data/twitter_cluster26_10M.oracleGeneral.bin', 'rb').read(n * 24)

def server_hash(k):  # server/store.cpp default_key_hash of the decimal key
    h = 0xcbf29ce484222325
    for c in str(k).encode():
        h = ((h ^ c) * 0x100000001b3) & mask
    h ^= h >> 30; h = (h * 0xbf58476d1ce4e5b9) & mask
    h ^= h >> 27; h = (h * 0x94d049bb133111eb) & mask
    return h ^ (h >> 31)

keys = sorted({rec.unpack_from(data, i * 24)[1] for i in range(n)})
maps = {'identity': {k: k for k in keys},
        'server_hash': {k: server_hash(k) for k in keys},
        'mult': {k: (k * 0x9E3779B97F4A7C15 + 1) & mask for k in keys}}
for seed in range(1, 10):
    perm = keys[:]
    random.Random(seed).shuffle(perm)
    maps[f'perm{seed}'] = dict(zip(keys, perm))
for name, m in maps.items():
    buf = bytearray(n * 24)
    for i in range(n):
        t, k, s, nx = rec.unpack_from(data, i * 24)
        rec.pack_into(buf, i * 24, t, m[k], s, nx)
    open(f'{out}/{name}.bin', 'wb').write(buf)
PY

echo "labeling,policy,capacity,hit_rate" >"$OUT"
for f in "$TMP"/*.bin; do
  for c in 1MiB 4MiB 8MiB 16MiB; do
    build/release/apps/cachesim/cachesim --trace "$f" -f oracle_general -p wtinylfu,lru -c "$c" |
      tail -2 | awk -F, -v OFS=, -v l="$(basename "$f" .bin)" '{print l,$2,$3,$7}' >>"$OUT"
  done
done
column -s, -t "$OUT"
