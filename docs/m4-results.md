# Milestone 4 results: defensive fallback (and the D20 feature pass before it)

Measured 2026-10-01/02, release build, i7-12700H (WSL2). All learned models here are 32 trees × 31
leaves, k = 64, compiled predictor, trained on LRU + random collector data (D18).

**Splits.** Twitter cluster26 (10M requests): train 0–5M, dev 5M–7.5M, test 7.5M–10M.
cloudPhysicsIO (113,872 requests): train 0–56,936, dev 56,936–85,404, **test 85,404–113,872**.
cloudPhysicsIO's test split is new (M3 used the whole second half), so its baselines differ from
docs/m3-results.md. Settings were chosen on dev; test was run afterwards.

## 1. Window scaled with the cache and normalized features (D20)

Scripts: `tools/m3_features.sh` (dev), `tools/m4_eval.sh` (test). Variants: **old** = fixed 10k
window, raw features; **r2raw** = window 2 × cached objects, raw; **r1n / r2n / r4n** = window
1 / 2 / 4 × cached objects, normalized.

**Dev, hit rate (%):**

| config | best baseline | old | r2raw | r1n | r2n | r4n |
|---|---|---|---|---|---|---|
| Twitter 512 KiB | W-TinyLFU 50.70 | 44.90 | **45.16** | 44.25 | 44.38 | 43.95 |
| Twitter 1 MiB | random 58.60 | 60.88 | 60.25 | 58.87 | 58.69 | **61.98** |
| Twitter 4 MiB | LRU 91.27 | **91.14** | 91.11 | 90.79 | 90.56 | 90.06 |
| cloudPhysicsIO 64 MiB | W-TinyLFU 21.47 | 19.93 | 19.89 | 20.42 | 20.13 | **20.86** |
| cloudPhysicsIO 256 MiB | W-TinyLFU 34.32 | 24.35 | 24.41 | **27.84** | 27.30 | 27.46 |
| cloudPhysicsIO 1 GiB | random 52.75 | 49.54 | 46.53 | **53.14** | **53.14** | **53.14** |

- Scaling the window alone (r2raw) changes almost nothing; normalization helps cloudPhysicsIO
  (up to +3.5 pp) and hurts Twitter at 4 MiB and, with ratio 1–2, at 1 MiB.
- **None of the variants fixes Twitter 512 KiB** (5.5–6.8 pp below W-TinyLFU). There the model is
  weak offline too: on its validation data it picks a truly furthest candidate 29.74% of the
  time vs 27.30% for sampled LRU (76.73% vs 62.99% at 1 MiB), so it isn't a run-time shift.
- **Chosen: r4n**, the best average over the six configurations (+1.12 pp per configuration over
  old).

**Test, learned policy alone, hit rate (%):**

| config | LRU | W-TinyLFU | random | old | **r4n** | Belady |
|---|---|---|---|---|---|---|
| Twitter 512 KiB | 49.57 | 49.11 | 49.50 | 45.94 | 44.40 | 64.42 |
| Twitter 1 MiB | 51.57 | 55.58 | 60.23 | 65.21 | 65.28 | 81.97 |
| Twitter 4 MiB | 91.94 | 82.11 | 87.99 | 91.60 | 90.73 | 92.38 |
| cloudPhysicsIO 64 MiB | 13.88 | 17.43 | 14.05 | 16.57 | 17.37 | 22.09 |
| cloudPhysicsIO 256 MiB | 20.80 | 29.69 | 21.52 | 23.37 | 29.26 | 39.00 |
| cloudPhysicsIO 1 GiB | 40.23 | 65.41 | 46.33 | 48.06 | 45.49 | 72.94 |

On test, r4n vs old: +5.89 pp at cloudPhysicsIO 256 MiB (now 0.43 pp from W-TinyLFU), +0.80 at
64 MiB, +0.07 at Twitter 1 MiB; −1.54 at Twitter 512 KiB, −0.87 at 4 MiB, −2.57 at
cloudPhysicsIO 1 GiB. **Path A helps where it was weakest on cloudPhysicsIO, but it is not a
general fix**; the learned policy alone still loses at Twitter 512 KiB and cloudPhysicsIO 1 GiB.

## 2. The fallback guard (D21)

`-p guarded`: the learned policy and a W-TinyLFU both track the cache, a shadow W-TinyLFU gives
the counterfactual, and a circuit breaker switches between them (window 10k requests, margin 0,
cooldown 50k with backoff, early 3√n check, grace period after each switch to learned).

### Choosing the grace period (dev, r4n models; `tools/m4_guard_dev.sh`)

The first version (no grace) tripped 38–43 times on Twitter 1 MiB test, where the model is about
10 pp better than W-TinyLFU, and lost 3.7 pp vs the model alone. After each switch to the learned
policy, the cache it inherits was shaped by W-TinyLFU, and it trails the shadow while it reshapes
it. Dev hit rate (%):

| config | W-TinyLFU | learned | grace 0 | grace 10k | **grace 30k** | 10k, no early check |
|---|---|---|---|---|---|---|
| Twitter 512 KiB | 50.70 | 43.95 | 49.12 | 50.36 | 49.16 | 48.62 |
| Twitter 1 MiB | 54.91 | 61.98 | 54.33 | 58.18 | 60.56 | 58.36 |
| Twitter 4 MiB | 81.64 | 90.06 | 90.88 | 90.87 | 90.87 | 90.87 |
| cloudPhysicsIO 64 MiB | 21.47 | 20.86 | 21.32 | 21.32 | 21.84 | 20.72 |
| cloudPhysicsIO 256 MiB | 34.32 | 27.46 | 31.49 | 27.04 | 27.04 | 27.04 |
| cloudPhysicsIO 1 GiB | 50.01 | 53.14 | 49.67 | 49.67 | 49.65 | 49.67 |

Criterion (set before looking): smallest total shortfall vs the better of learned and W-TinyLFU,
summed over the six configurations: grace 30k −12.55 pp, 10k −14.23, 0 −14.86, 10k without early
check −16.59. **Grace 30k is the default.** The trade-off is inherent: grace prevents false trips
where the model is good, and delays protection where it is bad.

### Test (r4n models, defaults, no time budget; `tools/m4_test.sh`)

| config | W-TinyLFU | learned (r4n) | **guarded** | trips / retries over the whole replay |
|---|---|---|---|---|
| Twitter 512 KiB | 49.11 | 44.40 | **48.77** | 21 / 20 |
| Twitter 1 MiB | 55.58 | 65.28 | **63.21** | 48 / 47 |
| Twitter 4 MiB | 82.11 | 90.73 | **91.81** | 7 / 7 |
| cloudPhysicsIO 64 MiB | 17.43 | 17.37 | **15.69** | 1 / 1 |
| cloudPhysicsIO 256 MiB | 29.69 | 29.26 | **16.69** | 1 / 1 |
| cloudPhysicsIO 1 GiB | 65.41 | 45.49 | **61.91** | 1 / 1 |

- **Where the model loses, the guard recovers most of the gap:** Twitter 512 KiB +4.37 pp over
  the model (0.34 pp below W-TinyLFU), cloudPhysicsIO 1 GiB +16.42 pp (3.50 pp below W-TinyLFU).
- **Where the model wins, the guard keeps most of the gain:** Twitter 1 MiB +7.63 pp over
  W-TinyLFU (2.07 pp below the model alone); Twitter 4 MiB is 1.08 pp *above* the model.
- **It fails on cloudPhysicsIO 64 MiB and 256 MiB**, ending below both policies (−1.74 and
  −13.00 pp vs W-TinyLFU).

### Why cloudPhysicsIO 256 MiB fails (diagnosis on the same runs)

- A guard that never trips (`--guard-window 1000000`) reproduces the learned policy exactly
  (29.26%), so the wrapper's dual tracking does not change its decisions.
- Tripping once early and never retrying gives 28.52%, close to W-TinyLFU: W-TinyLFU takes over a
  learned-shaped cache without trouble.
- The trace is strongly phased: the learned policy's hit rate on successive 10k-request segments
  is 44.19, 14.68, 19.98, 32.46, 14.06, 53.12, 37.39, 19.81, 18.39% (from request 0), and the
  test split is only 28,468 requests, shorter than grace + window + cooldown.
- In the failing run the single retry lands at the start of the test split, and the learned
  policy then runs on a cache W-TinyLFU shaped, unjudged during the 30k grace period. The
  measured result (16.69%) is far below the learned policy on its own cache (29.26%): **the
  model does badly on a W-TinyLFU-shaped cache**, which its training data (LRU- and
  random-shaped caches) never contained. The repeated trips on Twitter 1 MiB are likely the same
  effect. Not yet fixed; the natural fix is training data recorded from W-TinyLFU-shaped caches.

### Time budget (Twitter 1 MiB test, `--budget-us 100`; one run each, timing-dependent)

| model, k | learned alone | guarded, trip on 1 breach |
|---|---|---|
| 128 × 63, k = 64 | 68.08%, 594.21 s | 55.84%, 3.48 s: 200 learned evictions, all over budget; 4,521,198 by W-TinyLFU |
| 32 × 31, k = 16 | 64.66%, 34.53 s | 57.42%, 8.03 s: 176 latency trips in 559,141 learned evictions |

- With a model that is always too slow, the guard behaves like W-TinyLFU at W-TinyLFU speed.
- With a fast model (median about 11 µs), 0.03–0.05% of evictions still exceed 100 µs (likely
  scheduling jitter), and tripping on each one keeps the cache on W-TinyLFU most of the time.
  Breaches were first counted since the last switch, so even 10 allowed breaches added up
  (61.60%). **They are now counted within a window of the last 1,000 learned evictions**
  (`--breach-window`):

| `--max-breaches` (per 1,000 evictions) | hit rate | latency trips | hit-rate trips |
|---|---|---|---|
| 1 (default, "at once") | 57.57% | 179 | 0 |
| 3 | 58.23% | 22 | 21 |
| 10 | 62.35% | 1 | 44 |

With the window, latency trips nearly disappear at 10 per 1,000; the remaining losses are
hit-rate trips (the W-TinyLFU-shaped-cache problem above). The default stays "trip on the first
breach" as specified; 3–10 per 1,000 is the measured better setting when timing jitter is present.

### Overhead of the guard (`bench/eviction_bench`, held in learned mode)

Median µs per eviction + re-insert, 5 repetitions, same run: learned vs guarded, 32 × 31:
7.97 vs 7.82 (k = 16), 15.84 vs 15.12 (k = 32), 30.09 vs 32.72 (k = 64, 13% variation);
128 × 63, k = 16: 40.88 vs 38.75. **The wrapper's overhead is within measurement noise.** (This
run's absolute numbers are 20–30% lower than the step 8 run's, e.g. LRU 45.3 vs 63.5 ns: compare
only within one run.)

### Tests

`tests/policy/fallback_test.cpp`, 14 tests (plus a factory test): a good model never trips and changes nothing (same
hits as unguarded); a deliberately bad model (evicts the soonest-needed key) trips, early, and
recovers ≥ 90% of W-TinyLFU's hits; retries with backoff; grace and no-early-check behaviour;
shadow and real window counts match independent replays; the two inner policies stay in sync
through hundreds of switches with variable sizes; latency spikes trip at once (fake clock), the
breach window counts a rate, rare spread-out breaches don't trip, no budget means no latency
trips, and retries after the cooldown. Full suite: 211 tests pass under debug, ASan + UBSan and
TSan.

## Summary

- Milestone 4's done criterion (a test with a deliberately bad model shows fallback kicking in)
  is met, on unit tests and on real traces.
- The guard turns the worst learned losses into small ones (Twitter 512 KiB −4.71 → −0.34 pp vs
  W-TinyLFU; cloudPhysicsIO 1 GiB −19.92 → −3.50 pp) while keeping most of the learned policy's
  win at Twitter 1 MiB (+7.63 pp vs W-TinyLFU), at negligible overhead.
- **Open:** the learned policy does badly on W-TinyLFU-shaped caches, which makes switching back
  costly (cloudPhysicsIO 256 MiB guarded: 16.69% vs 29.26% learned alone and 29.69% W-TinyLFU);
  guard settings are request counts and are out of scale on the 113k-request cloudPhysicsIO
  trace; timing jitter needs `--max-breaches` > 1.
