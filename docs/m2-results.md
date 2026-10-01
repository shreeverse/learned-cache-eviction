# Milestone 2 results: LRU, LFU, ARC, W-TinyLFU, Belady

All numbers below were measured on 2026-10-01 with the `release` build. Reproduce them with:

```
tools/fetch_traces.sh
build/release/apps/cachesim/cachesim -t data/cloudPhysicsIO.oracleGeneral.bin \
    -p lru,lfu,arc,wtinylfu,belady -c 64MiB,256MiB,1GiB
build/release/apps/cachesim/cachesim -t data/cloudPhysicsIO.oracleGeneral.bin \
    -p lru,lfu,arc,wtinylfu,belady -c 100,1000,10000 --ignore-size
```

**Trace:** `cloudPhysicsIO.oracleGeneral.bin` (block I/O, 113,872 requests, 48,974 unique objects,
4,368,040,448 bytes requested). One small trace; a Twitter/Wikipedia trace comes with M3.

## Hit rate (byte-sized cache)

| policy | 64 MiB | 256 MiB | 1 GiB |
|---|---|---|---|
| LRU | 17.27% | 21.15% | 37.03% |
| LFU | 18.36% | 23.84% | 43.47% |
| ARC | 18.45% | 26.24% | 43.47% |
| W-TinyLFU | 19.73% | 29.06% | 47.65% |
| Belady | 24.74% | 37.93% | 56.99% |

## Byte hit rate (byte-sized cache)

| policy | 64 MiB | 256 MiB | 1 GiB |
|---|---|---|---|
| LRU | 2.53% | 7.02% | 29.91% |
| LFU | 3.04% | 10.32% | 38.27% |
| ARC | 3.19% | 13.01% | 38.28% |
| W-TinyLFU | 4.96% | 17.99% | 40.72% |
| Belady | 13.25% | 31.43% | 53.53% |

## Hit rate (unit-size objects, `--ignore-size`)

| policy | 100 objects | 1,000 objects | 10,000 objects |
|---|---|---|---|
| LRU | 11.99% | 16.73% | 30.24% |
| LFU | 11.33% | 16.08% | 28.82% |
| ARC | 14.53% | 17.43% | 30.26% |
| W-TinyLFU | 12.82% | 17.93% | 34.78% |
| Belady | 17.46% | 23.58% | 45.69% |

## What the numbers show (measured, this trace only)

- **Belady is highest in every configuration.** At 1 GiB it reaches 64,898 hits, which is every
  non-compulsory request (113,872 requests − 48,974 first accesses), so it's the ceiling for any policy.
- **W-TinyLFU is the best non-oracle policy** in all three byte-sized configurations and at 1,000 and
  10,000 objects. **ARC is best at 100 objects.**
- **The gap to Belady is the headroom M3 targets:** at 256 MiB, W-TinyLFU is 29.06% vs Belady 37.93%
  (8.87 pp).
- **In-cache LFU is below LRU** in all three unit-size configurations (it forgets frequency on
  eviction and has no aging), but above LRU in all three byte-sized ones.

## Cross-check against libCacheSim (commit 93529a1)

Produced by `tools/validate_vs_libcachesim.sh`, which fails unless every enforced check holds.

| policy | cache size | unit | libCacheSim hits | our hits | libCacheSim hit rate | our hit rate | diff (pp) | check |
|---|---|---|---|---|---|---|---|---|
| lru | 67108864 | bytes | 19669 | 19669 | 0.172729 | 0.172729 | +0.0000 | exact |
| lru | 268435456 | bytes | 24089 | 24089 | 0.211545 | 0.211545 | +0.0000 | exact |
| lru | 1073741824 | bytes | 42168 | 42168 | 0.370311 | 0.370311 | +0.0000 | exact |
| lru | 100 | objects | 13657 | 13657 | 0.119933 | 0.119933 | +0.0000 | exact |
| lru | 1000 | objects | 19049 | 19049 | 0.167284 | 0.167284 | +0.0000 | exact |
| lru | 10000 | objects | 34434 | 34434 | 0.302392 | 0.302392 | +0.0000 | exact |
| lfu | 67108864 | bytes | 20907 | 20907 | 0.183601 | 0.183601 | +0.0000 | exact |
| lfu | 268435456 | bytes | 27152 | 27152 | 0.238443 | 0.238443 | +0.0000 | exact |
| lfu | 1073741824 | bytes | 49496 | 49496 | 0.434663 | 0.434663 | +0.0000 | exact |
| lfu | 100 | objects | 12899 | 12899 | 0.113276 | 0.113276 | +0.0000 | exact |
| lfu | 1000 | objects | 18310 | 18310 | 0.160795 | 0.160795 | +0.0000 | exact |
| lfu | 10000 | objects | 32813 | 32813 | 0.288157 | 0.288157 | +0.0000 | exact |
| arc | 67108864 | bytes | 21300 | 21008 | 0.187052 | 0.184488 | -0.2564 | info |
| arc | 268435456 | bytes | 28011 | 29876 | 0.245987 | 0.262365 | +1.6378 | info |
| arc | 1073741824 | bytes | 49100 | 49505 | 0.431186 | 0.434743 | +0.3557 | info |
| arc | 100 | objects | 16542 | 16542 | 0.145268 | 0.145268 | +0.0000 | exact |
| arc | 1000 | objects | 19845 | 19845 | 0.174275 | 0.174275 | +0.0000 | exact |
| arc | 10000 | objects | 34459 | 34459 | 0.302612 | 0.302612 | +0.0000 | exact |
| wtinylfu | 67108864 | bytes | 21498 | 22470 | 0.188791 | 0.197327 | +0.8536 | info |
| wtinylfu | 268435456 | bytes | 29683 | 33088 | 0.260670 | 0.290572 | +2.9902 | info |
| wtinylfu | 1073741824 | bytes | 60215 | 54257 | 0.528795 | 0.476474 | -5.2322 | info |
| wtinylfu | 100 | objects | 15073 | 14596 | 0.132368 | 0.128179 | -0.4189 | info |
| wtinylfu | 1000 | objects | 18961 | 20418 | 0.166512 | 0.179307 | +1.2795 | info |
| wtinylfu | 10000 | objects | 36591 | 39604 | 0.321334 | 0.347794 | +2.6460 | info |
| belady | 67108864 | bytes | 28113 | 28173 | 0.246882 | 0.247409 | +0.0527 | info |
| belady | 268435456 | bytes | 43148 | 43196 | 0.378917 | 0.379338 | +0.0422 | info |
| belady | 1073741824 | bytes | 64898 | 64898 | 0.569921 | 0.569921 | +0.0000 | exact |
| belady | 100 | objects | 19862 | 19877 | 0.174424 | 0.174556 | +0.0132 | ours >= ref |
| belady | 1000 | objects | 26847 | 26853 | 0.235765 | 0.235817 | +0.0053 | ours >= ref |
| belady | 10000 | objects | 52029 | 52030 | 0.456908 | 0.456917 | +0.0009 | ours >= ref |

failed checks: 0

- **LRU, LFU:** exact on all 12 configurations (hits and bytes).
- **ARC:** exact with unit sizes. With byte sizes the two implementations make different
  choices about ghost bookkeeping (decisions.md D8), so they differ by −0.26 to +1.64 pp.
- **Belady:** ours is ≥ libCacheSim everywhere, because ours may decline to cache an object it will never reuse
  (with unit sizes that's the true optimum, verified against exhaustive search in the tests).
  Both reach the ceiling at 1 GiB.
- **W-TinyLFU:** different designs (count-min sketch with aging vs. a counting Bloom filter sized
  in bytes that, at these cache sizes, effectively never collides or decays). Ours is higher in 4 of 6
  configurations, and 5.23 pp lower at 1 GiB. See D9 for an experiment on our sketch's sizing.
