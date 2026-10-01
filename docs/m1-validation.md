# Milestone 1 validation: LRU vs libCacheSim

**Result: exact match.** On every configuration below, our simulator and libCacheSim's LRU report
the same number of requests and hits, and the same request and hit byte counts where sizes are used.
The largest hit-rate difference is **0.0000 percentage points** (the target was ≤ 0.5 pp).

## Setup

| | |
|---|---|
| Trace | `cloudPhysicsIO.oracleGeneral.bin` from libCacheSim's `data/` (113,872 requests, 48,974 unique objects, sha256 `65438dd7…6050dea`) |
| Reference | libCacheSim commit `93529a1faef2c57bbdb9323213b78cad2adec1cd`, built Release with `OPT_SUPPORT_ZSTD_TRACE=OFF` |
| Ours | `cachesim` from this repo, `release` preset (GCC 11.4) |
| Policy | LRU, no warmup, all requests treated as GETs |
| Date | 2026-10-01 |

libCacheSim's `cachesim` binary prints miss ratios with only 4 decimals, which can't show an exact
match. So `tools/libcachesim_exact.c` drives libCacheSim's library API directly (the same trace
reader and `LRU_init`) and prints raw counters.

## Results

Reproduce with `tools/validate_vs_libcachesim.sh`.

| cache size | unit | requests | libCacheSim hits | our hits | libCacheSim hit rate | our hit rate | diff (pp) |
|---|---|---|---|---|---|---|---|
| 67108864 (64 MiB) | bytes | 113872 | 19669 | 19669 | 0.172729 | 0.172729 | +0.0000 |
| 268435456 (256 MiB) | bytes | 113872 | 24089 | 24089 | 0.211545 | 0.211545 | +0.0000 |
| 1073741824 (1 GiB) | bytes | 113872 | 42168 | 42168 | 0.370311 | 0.370311 | +0.0000 |
| 100 | objects | 113872 | 13657 | 13657 | 0.119933 | 0.119933 | +0.0000 |
| 1000 | objects | 113872 | 19049 | 19049 | 0.167284 | 0.167284 | +0.0000 |
| 10000 | objects | 113872 | 34434 | 34434 | 0.302392 | 0.302392 | +0.0000 |

Byte hit counts at the three byte capacities also match exactly: 110,606,336 / 306,798,080 /
1,306,377,728 bytes hit out of 4,368,040,448 requested. In object mode (`--ignore-size`), byte
counts are not compared, because libCacheSim counts every object as 1 byte there while we keep
real sizes for byte metrics (decisions.md D6).

As a check that the comparison can fail, changing one hit count in our output by 1 makes the
script report `exact match of all counters: False`.

## What made the exact match possible

- Size-0 requests are skipped the same way libCacheSim skips them (decisions.md D4). This trace has none,
  so it isn't exercised here.
- Objects larger than the cache are not inserted, and a key keeps its first size (D2, D3).

## Limits of this result

- One small trace (block I/O, 113k requests). A Twitter or Wikipedia trace comes in M2/M3.
- This only validates LRU; each new policy needs its own reference check.
- Environment note: this machine had no root access, so glib headers came from Ubuntu's
  `libglib2.0-dev` .deb unpacked into a temp dir via `apt-get download` + `dpkg -x`, and the script found
  them through `PKG_CONFIG_PATH`. On a normal machine, `apt install libglib2.0-dev` is enough.

Regression test: `tests/sim/libcachesim_golden_test.cpp` pins these counts (skipped if the trace
isn't downloaded).
