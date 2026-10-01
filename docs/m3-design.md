# Milestone 3 design: the learned policy

Goal: an eviction policy that predicts each cached object's time to next access with a LightGBM
model and evicts the sampled candidate predicted to be needed furthest in the future (the
"Learning Relaxed Belady" idea, Song et al., NSDI '20). Done when: a hit-rate comparison graph vs
W-TinyLFU and Belady.

## 0. Overall approach

```
 trace ──► annotate_next_access ──► Cache + CollectorPolicy ──► features.csv (features + label)
                                          │ uses
                                          ▼
                                    FeatureTracker  ◄─── same code ───┐
                                                                      │
 tools/train.py (LightGBM) ──► model.txt ──► LightGbmPredictor ──► LearnedPolicy
```

**Rule: one `FeatureTracker` class computes features for both the training-data collector and
the policy,** so training and inference features can't drift apart. Mismatched features are the
most common way learned systems fail without anyone noticing.

## 1. Feature extraction (`ml/feature_tracker.{h,cpp}`)

**Clock: virtual time.** The tracker counts the accesses it has seen. Access number i (0-based)
happens at time i, and `now()` is the time of the latest access. Virtual time means the same
thing on every trace (timestamps don't), and it uses the same units as `Request::next_vtime`, which
the labels need. (The Cache skips the policy for objects larger than the whole cache, so the
tracker's clock can fall behind the stream index by the number of such requests. That makes labels
slightly off only for those requests. D11.)

**Per-key history.** Fixed size, no heap allocation per key:

```cpp
struct KeyHistory {
  std::uint64_t last_vtime;
  std::uint32_t size;                          // size at first access (like the Cache, D3)
  std::uint32_t count;                         // accesses while tracked, saturating
  std::array<std::uint32_t, kNumDeltas> deltas; // ring buffer of gaps between accesses, saturating
  std::array<float, kNumEdcs> edc;             // exponentially decayed counters
  std::uint8_t delta_head, delta_len;
};
```

**Features (15, in this order):**

| feature | meaning |
|---|---|
| `log_size` | log2(size), size 0 treated as 1 |
| `age` | now − last_vtime |
| `delta_1..8` | the most recent gap between accesses first; missing ones are NaN, which LightGBM handles natively |
| `count` | accesses seen while tracked |
| `edc_1..4` | on access `edc ← 1 + edc · 2^(−Δ/h)`; reported decayed to now, `edc · 2^(−age/h)`; h = 2^9, 2^11, 2^13, 2^15 |

These are LRB's features, with fewer deltas and counters (LRB used 32 and 10), to keep inference
cheap. What dropping them costs can be measured later.

**History is kept only for tracked (cached) keys** (D13). Evicting a key drops its history; LRB's
memory window for evicted keys is deferred and will be a measured experiment.

Cost: O(1) per access, O(F) per feature row, one hash map entry of about 64 bytes per cached key.

## 2. Inference (`ml/predictor.h`)

```cpp
class Predictor {
 public:
  virtual ~Predictor() = default;
  // keys.size() rows of kNumFeatures features each, row-major. Writes one score per row:
  // higher = next access predicted further away (= better victim).
  virtual void predict(std::span<const Key> keys, std::span<const float> features,
                       std::span<double> scores) = 0;
  // Called for every request the policy sees, before the feature update.
  virtual void observe(const Request&) {}
  // True if observe() reads Request::next_vtime (oracle); the policy then reports it upward.
  virtual bool needs_next_access() const { return false; }
};
```

**Change from the original proposal:** `predict` also gets the candidates' keys, and there's an
`observe` hook. The model ignores both. But the oracle needs to know *which* key each row is and
when that key is next requested, and tests are much clearer when a fake model can score by key.

Implementations:

- **`OraclePredictor`**: the score is the key's true next access (from `observe`); "never again"
  scores highest. `LearnedPolicy` with this predictor is **sampled Belady**: the best that
  sampling k candidates can do with perfect predictions.
- **`RandomPredictor`**: random scores, so it evicts a random sampled candidate. Lower bound.
- **`FunctionPredictor`**: wraps a lambda, for tests and for M4's deliberately bad models.
- **`LightGbmPredictor`** (step 7): `LGBM_BoosterCreateFromModelfile`, then one batched
  `LGBM_BoosterPredictForMat` per eviction with `num_threads=1`. At load it checks the model's
  feature names against `FeatureTracker::feature_names()` and throws on a mismatch.

**Model:** LightGBM regression on `log1p(next_vtime − now)` (D12). For keys never requested again
the label is capped at `log1p(2 × trace length)`. Starting size: about 32 trees, 31 leaves.

**Build:** LightGBM v4.x through FetchContent, pinned by hash, behind `LC_WITH_LIGHTGBM`, built with
`USE_OPENMP=OFF` (D14).

## 3. `LearnedPolicy` (`policy/learned.{h,cpp}`)

- `std::vector<Key> keys_` plus `std::unordered_map<Key, std::uint32_t> pos_`: O(1) uniform
  sampling, O(1) removal by swapping with the last element.
- Owns a `FeatureTracker` and a `std::unique_ptr<Predictor>`. Owning the predictor is what lets
  `make_policy` return one self-contained object. Tests keep a raw pointer to inspect it.
- Reused buffers for candidate keys, features (k·F floats) and scores; `std::mt19937_64` with a
  fixed seed, so runs are deterministic.

Hooks:

- `on_hit` / `on_insert`: `predictor.observe(r)`, `tracker.on_access(key, size)`; insert also
  appends to `keys_`.
- `evict()`: if at most k keys are tracked, every key is a candidate; otherwise sample k uniformly
  with replacement. Write the feature rows, make **one** `predict` call, and evict the highest
  score (ties: smallest key). Cost: O(k·F) + one model call.
- The newcomer may be the victim if it is sampled (admission-like behavior, consistent with D2).
- `on_remove`: the same removal path as eviction.

**Fallbacks in M3:** a missing or mismatched model is a hard error at construction time. Keys with
little history are handled by NaN features. M4 adds a `FallbackPolicy` wrapper (a shadow
W-TinyLFU, rolling hit rates, a time budget per eviction) around `LearnedPolicy` without changing it.

## 4. Factory and simulator

```cpp
struct PolicyConfig {
  std::uint64_t capacity_bytes = 0;
  std::string model_path;          // "learned" only (step 7)
  std::uint32_t sample_size = 64;  // sampled policies
  std::uint64_t seed = 1;          // sampled policies
};
std::unique_ptr<EvictionPolicy> make_policy(std::string_view name, const PolicyConfig& config);
```

The old `make_policy(name, capacity)` stays as a shorthand. New names: `oracle-sampled`,
`random-sampled` (and `learned` in step 7).

`SimOptions::warmup_requests`: the first N requests are replayed but not counted, so every policy
is scored on the same held-out suffix (D10). `cachesim` gains `--warmup`, `--sample-size`, `--seed`.

## 5. Training data (step 5)

The collector is `LearnedPolicy` with a `RecordingPredictor` wrapped around an `AgePredictor`
(the oldest sampled candidate wins: sampled LRU). It's not a separate policy class, so the
sampling and feature code is literally the same as at inference. `lcache-export` runs it. At each eviction it writes one row per candidate: the features at that
moment and the label `next_vtime − now`. The candidate hasn't been accessed since its last access,
so that access's `next_vtime` is exactly its next access. The rows come from the same distribution
as inference rows: cached objects sampled at eviction time. Output: CSV with a header of feature
names. Known limitation: data comes from a sampled-LRU cache, not from the learned policy itself.

## 6. Evaluation protocol (D10)

Split by time: train on the first 50% of the trace; report hit rate on the last 50% only, for
every policy (`--warmup`). Lines in the graph:

| line | what it shows |
|---|---|
| W-TinyLFU | best classic policy |
| Learned | the model |
| oracle-sampled | best possible with k-candidate sampling |
| Belady | the optimum |
| random-sampled | lower bound |

Belady → oracle-sampled is the **cost of sampling**; oracle-sampled → learned is the **cost of
prediction error**. Inference latency per eviction will be measured with Google Benchmark for
k = 16/64/128.

## 7. Plan (one commit per step)

1. This document plus decisions D10–D14.
2. `FeatureTracker` with tests.
3. `Predictor` interface, oracle/random/function predictors, `LearnedPolicy` with tests.
4. `PolicyConfig`, factory, `--warmup`; first measured numbers: oracle-sampled vs Belady.
5. Collector (`RecordingPredictor` + `AgePredictor`), `lcache-export`, label test.
6. Python venv, `train.py`, first model.
7. LightGBM dependency, `LightGbmPredictor`, model-loading tests with a committed tiny model.
8. Latency benchmark.
9. Results, plot, `docs/m3-results.md`.
