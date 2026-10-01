# Milestone 3 results

All numbers below were measured with the `release` build. Hit rates are over the **second half**
of the trace only: the first half warms the cache and will be the training set (decisions D10).
So these numbers are **not comparable with the M2 tables**, which count every request from a cold
cache.

## Step 4: the cost of sampling (no model yet)

`oracle-sampled` is `LearnedPolicy` with perfect predictions: at each eviction it samples 64
cached keys and evicts the one whose true next access is furthest away. It is the best any
learned model can do with this candidate sampling. `random-sampled` evicts a random sampled key.
Measured 2026-10-01 at commit `90aa58d`:

```
build/release/apps/cachesim/cachesim -t data/cloudPhysicsIO.oracleGeneral.bin \
    -p lru,wtinylfu,random-sampled,oracle-sampled,belady -c 64MiB,256MiB,1GiB -w 56936
build/release/apps/cachesim/cachesim -t data/cloudPhysicsIO.oracleGeneral.bin \
    -p lru,wtinylfu,random-sampled,oracle-sampled,belady -c 100,1000,10000 --ignore-size -w 56936
```

**Trace:** `cloudPhysicsIO.oracleGeneral.bin`, 113,872 requests; warmup 56,936; 56,936 requests
counted. Sample size 64, seed 1. Two runs gave byte-identical output.

### Hit rate, byte-sized cache

| policy | 64 MiB | 256 MiB | 1 GiB |
|---|---|---|---|
| LRU | 16.54% | 20.36% | 36.58% |
| random-sampled | 16.33% | 21.41% | 49.54% |
| W-TinyLFU | 19.45% | 32.01% | 57.71% |
| oracle-sampled | 23.59% | 37.57% | 76.02% |
| Belady | 23.90% | 38.12% | 76.24% |

### Hit rate, unit-size objects (`--ignore-size`)

| policy | 100 objects | 1,000 objects | 10,000 objects |
|---|---|---|---|
| LRU | 11.03% | 15.81% | 29.49% |
| random-sampled | 10.21% | 15.07% | 25.92% |
| W-TinyLFU | 12.21% | 17.59% | 38.90% |
| oracle-sampled | 16.45% | 22.78% | 52.75% |
| Belady | 16.52% | 22.91% | 53.64% |

### What this shows (this trace only)

- **Sampling 64 candidates costs at most 0.89 pp of hit rate** compared with full Belady (0.07 to
  0.89 pp across the six configurations). So almost all of the headroom is still available to a
  sampled learned policy.
- **The headroom over W-TinyLFU with perfect predictions is 4.13 to 18.31 pp** (4.13 / 5.57 /
  18.31 pp byte-sized; 4.24 / 5.19 / 13.85 pp unit-size). How much of it a LightGBM model recovers
  is what the next steps measure.
- `random-sampled` is below LRU in 4 of 6 configurations, but well above it at 1 GiB (49.54% vs
  36.58%).
