# Design decisions

A running log of design decisions and the alternatives we rejected. Newest entries at the bottom.

## D1. Policy interface: hooks + a shared `Cache` (M1)

**Decision.** `EvictionPolicy` exposes hooks (`on_hit`, `on_insert`, `evict`, `on_remove`). `sim::Cache`
owns the key→size map and byte accounting and asks the policy for victims.

**Rejected.** The libCacheSim-style `bool access(req)`, where each policy manages its own capacity.

**Why.**
- Capacity semantics (byte accounting, objects larger than the cache) are implemented once, so every policy is
  compared under identical rules.
- Eviction order can be unit-tested directly by calling `evict()`.
- The same policies can be reused by the network server (M5), whose store holds values.

**Cost.** Two hash maps per access (Cache + policy). Acceptable for simulation; revisit if profiling shows it.

## D2. Insert, then evict (M1)

On a miss the Cache calls `on_insert` first and then `evict()` until `used <= capacity`. This lets admission-
style policies (W-TinyLFU, Learned) choose the newcomer itself as the victim. For LRU the newcomer is the MRU
item, so this is equivalent to libCacheSim's evict-then-insert order. Objects larger than the whole cache are
never inserted (counted as a miss).

## D3. Request semantics follow libCacheSim (M1)

- Every request is treated as a GET; a miss inserts the object.
- A key's size is the size of its first request; later size changes on hits are ignored.
- No warmup period; hit rate is over all requests.

These match libCacheSim so the M1 LRU cross-check can be an exact comparison.

## D4. Trace formats and keys (M1)

- Primary format: libCacheSim `oracleGeneral` binary — 24-byte little-endian records
  `{u32 time, u64 obj_id, u32 obj_size, i64 next_access_vtime}`. Verified against libCacheSim's
  `oracleGeneralBin.h` and `data/cloudPhysicsIO.oracleGeneral.bin` at commit `93529a1`. In that file
  `next_access_vtime` is 1-based (the next access's 0-based index + 1) and -1 means "never again".
- **Requests with size 0 are skipped by default**, because libCacheSim's reader does the same
  (`ignore_size_zero_req = true`). Without this, request counts would not match.
- `next_access_vtime` is not stored in `Request` for now; Belady (M2) and ML labels (M3) will compute
  next-access times in their own pass, so they also work on formats that don't include it.
- Fields are read with `memcpy`, never by casting the buffer pointer, which would be an unaligned access (UB).
- String keys (CSV) are hashed with FNV-1a 64-bit, not `std::hash`, so results are the same on every
  platform and standard library.

## D5. Running TSan on WSL2 (tooling)

The TSan runtime shipped with GCC 11 / LLVM 14 aborts on this kernel with
`FATAL: ThreadSanitizer: unexpected memory mapping`, because the kernel uses more ASLR entropy than that
runtime supports. Workaround without root: run TSan tests with ASLR disabled for that process:

    setarch "$(uname -m)" -R ctest --preset tsan

GoogleTest discovery is set to `PRE_TEST` so the build itself never has to execute a TSan binary.
(Alternatives: `sudo sysctl vm.mmap_rnd_bits=28`, or a newer compiler whose TSan supports this.)

## D6. Simulator details (M1)

- `Cache` holds a non-owning reference to its policy, so tests can inspect a fake policy directly.
  The policy must be fresh for every run; `simulate()` rewinds the reader so one reader can be replayed
  for several cache sizes.
- `ignore_size` sets every object's size to 1, so the capacity counts objects (libCacheSim's `--ignore-obj-size`).
  Byte hit rate still uses the real sizes, so it stays meaningful.
- `Cache` throws `std::logic_error` if a policy evicts a key the cache doesn't hold, rather than
  silently corrupting the byte count or looping. This guards against bugs in the learned policy (M3/M4).

## D7. `Request::next_vtime` and the annotation pre-pass (M2; revises D4)

D4 kept next-access information out of `Request`. Belady (M2) and the M3 labels both need it, so
`Request` now has `next_vtime`, filled by `annotate_next_access()`: load the trace into memory and
walk it backwards with a last-seen map. The oracleGeneral file's own field is still ignored: its
indices count the size-0 records we skip, and CSV traces don't have it. Cost: O(N) memory,
about 24 bytes per request in memory. Very large traces will need sampling.

## D8. ARC with variable sizes differs from libCacheSim's ARC (M2)

Both match the paper (and each other) exactly with unit sizes. With byte sizes, ours differs from
libCacheSim by −0.26 to +1.64 pp on cloudPhysicsIO. The paper is defined for unit pages, so byte
ARC has no canonical form:
- **Ours:** insert-then-evict; ghost lists trimmed to the paper's bounds (|T1|+|B1| ≤ c,
  total ≤ 2c, in bytes) once the resident set fits; p moves by `size × max(1, ratio)`.
- **libCacheSim:** evict-then-insert; deletes at most one ghost per miss; p moves by
  `max(1, ratio)` bytes regardless of object size.

**Tested hypothesis (rejected):** p's step size is not the cause. With libCacheSim's unscaled step,
ours gives 21,008 / 30,242 / 49,505 hits vs libCacheSim's 21,300 / 28,011 / 49,100, no closer.
We keep the size-scaled step: with unit sizes it is exactly the paper's δ.

## D9. W-TinyLFU sketch growth: forget counts (kept) vs keep counts (rejected) (M2)

The sketch starts 16 wide and doubles when resident keys exceed its width. Because widths are
powers of two, doubling *could* keep every estimate (copy each counter into column j and
j + old width). We measured both on cloudPhysicsIO (hits):

| config | forget on growth (kept) | keep counts | change |
|---|---|---|---|
| 64 MiB | 22,470 | 22,906 | +0.38 pp |
| 256 MiB | 33,088 | 26,802 | −5.52 pp |
| 1 GiB | 54,257 | 59,552 | +4.65 pp |
| 100 objects | 14,596 | 14,797 | +0.18 pp |
| 1,000 objects | 20,418 | 20,069 | −0.31 pp |
| 10,000 objects | 39,604 | 30,530 | −7.97 pp |

Keeping counts is not better overall: it wins 3 configurations and loses 3, by up to 7.97 pp.
Likely cause: in the narrow early sketch most counters saturate from collisions, and keeping them
makes old keys look maximally popular, so new candidates are rejected until aging catches up.
Picking whichever variant suits this one trace would be overfitting, so the original design stays.
Revisit when sizing the sketch from an object-count estimate, or with Caffeine's adaptive window (M3+).

## D10. Evaluation by time split, with warmup (M3)

The learned policy is trained on the first 50% of a trace and every policy is scored only on the
last 50% (`SimOptions::warmup_requests`: the first N requests are replayed to warm the cache but not
counted). Scoring on the training requests would overstate the model. Classic policies are scored
the same way, so the comparison is fair. The M2 tables (no warmup) are not comparable with M3 tables.

## D11. Features use virtual time (M3)

Ages and gaps between accesses are counted in requests seen by the policy, not trace timestamps.
Virtual time means the same thing on every trace and uses the same units as `next_vtime`, so labels
are `next_vtime − now`. Caveat: the Cache never shows the policy objects larger than the whole
cache, so the policy's clock can fall behind the stream index by that many requests.

## D12. Label: regression on log1p(time to next access) (M3)

**Rejected:** LRB's binary label ("next access beyond the Belady boundary"). Regression gives a
ranking directly, which is what choosing a victim among candidates needs, and has no boundary
parameter to tune. The log keeps the far-future tail from dominating the squared loss. Keys never
requested again are capped at log1p(2 × trace length).

## D13. Feature history only for cached keys (M3, v1)

`FeatureTracker` drops a key's history when it is evicted, so a key that comes back starts with
no gaps (NaN features). LRB keeps a bounded memory window of history for evicted keys too; that is
deferred and will be measured, not assumed.

## D14. Predictor interface sees keys, and policies own their predictor (M3)

`Predictor::predict` gets the candidates' keys as well as their features, and an `observe(Request)`
hook. A LightGBM model ignores both, but they let the same `LearnedPolicy` run with an oracle
(sampled Belady, which separates the cost of sampling from the cost of prediction error), with
random scores (lower bound), and with fake models scored by key in tests. LightGBM will be built
with `USE_OPENMP=OFF`: inference for one batch per eviction doesn't need threads, and LightGBM's
OpenMP pool would give TSan false positives.

## D15. Second trace: first 10M requests of Twitter cluster26 (M3)

cloudPhysicsIO (113,872 requests) is too small to train a model on half of it. The Twitter traces
are large (cluster26, the smallest, is 784 MB compressed; cluster52 is 92 GB), so
`tools/fetch_traces.sh` keeps the **first 10M requests** of cluster26 as an uncompressed 240 MB
file, checksum-pinned. **Rejected:** random or spatial sampling, which would break the time order
that the time split (D10) depends on. Measured stats of the prefix: 10,000,000 requests,
778,770 unique objects (median 145 bytes), 104 MiB working set, about 11 hours of traffic, and
92.2% hit rate for an infinite cache.

## D16. LightGBM 4.5.0, not the latest (M3)

LightGBM 4.6.0 and 4.7.0 require CMake 3.28; Ubuntu 22.04 (this machine and the planned CI image)
ships 3.22. 4.5.0 needs 3.18. The model file format is shared across 4.x, but Python training and
C++ inference both pin **4.5.0** so the trainer and the predictor are the same code. The C++ side
builds from the 4.5.0 Python sdist (1.7 MB, includes the C++ sources and vendored Eigen/fmt) rather
than the 175 MB "complete source" release asset. **Rejected:** upgrading CMake through pip, which
would make every build depend on a venv.

## D17. Memory window: keep history for recently evicted keys (M3; revises D13)

**Why D13 was reversed (measured, docs/m3-results.md "Diagnosis"):** with history dropped on
eviction, 83.5% of training candidates had a shorter feature history than the trace shows, and
92.1% of candidates with no gap history had been requested before. The features were mostly
describing "time since the last insert".

**Decision.** `FeatureTracker(history_capacity)` keeps the history of the N most recently
evicted keys ("ghosts"); the oldest is dropped first, and a key that comes back continues its
history (the time it spent evicted counts as a gap). `--history N` in `cachesim` and
`lcache-export`; 0 (the default) is D13's behavior.

**Rejected:** LRB's time-based window (drop history older than W requests). A count bound gives a
hard memory bound, which matters for a server (M5); for a steady request mix the two are roughly
equivalent.

**Cost and fairness caveat.** Each ghost costs an `unordered_map` node plus a `std::list` node,
on the order of 100–150 bytes. Like W-TinyLFU's sketch and ARC's ghost lists, this metadata is
not charged to the cache capacity. With Twitter's ~145-byte objects, a large window can use more
memory than a 1 MiB cache holds, which the results must say.

**Train/serve consistency.** A model must be run with the `--history` value its training data
was exported with; nothing checks this yet.

## D18. Training data and model for the learned policy (M3)

Chosen on a dev split (requests 5M–7.5M of Twitter cluster26) after the diagnosis in
docs/m3-results.md; evaluated once on a test split (7.5M–10M).

- **Training data: sampled-LRU collector + random collector, pooled.** LRU-collected data alone
  never contains old keys, so the model kept dead keys forever (distribution shift).
  **Rejected:** on-policy rounds (DAgger style): they help (59.50% → 61.20% over two rounds, on
  the whole second half) but need iteration, while adding random-cache data reached 63.05%
  in one step.
- **Model: 128 trees × 63 leaves, regression on log1p(distance)** (D12 stands). +3.34 pp over
  32 × 31 on dev, at about 4–5× the inference cost. **Rejected:** binary labels at horizons
  12k/25k, a clipped label, lambdarank (all equal or worse on dev), and 5× more rows (no gain).
- **Evaluation protocol: train / dev / test by time** (0–5M / 5M–7.5M / 7.5M–10M). Results
  before this change used the whole second half and were used to choose what to try next.

## D19. Our own tree evaluator instead of LightGBM's predictor (M3 step 8)

**Measured problem:** with LightGBM's C API, one eviction (k = 64 candidates) took 121 µs with the
32-tree model and 595 µs with the 128-tree model (`bench/eviction_bench`), vs 74 ns for LRU.

**Decision.** `TreeEnsemblePredictor` reads LightGBM's text model and evaluates it itself; it is
the default for `learned` (`--predictor compiled`; `--predictor lightgbm` remains). It
reproduces LightGBM's numerical decision rules and sums trees in the same order, so scores are
**bit-identical**: tests compare both on 20,000 rows for both committed models, and a dev-split
replay gives the same hits (1,522,004) with either predictor. Three steps, each measured on the
32 × 31 model at k = 64 (predict only):
1. Flat node array, one walk per row: 83 µs (LightGBM: 116–138 µs).
2. Leaves as self-loops so each walk takes a fixed number of steps; 8 rows walked through a tree
   together; NaN direction precomputed per split; 24-byte nodes: 65 µs.
3. No branches in a step (children indexed by the comparison result, bitwise conditions): 39 µs.
   Branch mispredictions on tree data were the main cost. 8 lanes beat 4 (57 µs) and 16 (44 µs).

**Rejected:** QuickScorer-style bitvector evaluation and code generation (one C++ function per
tree). Faster still, but much more code; revisit if M4's time budget needs it.

**Limits:** regression models with numerical splits only; anything else is rejected at load
(use `--predictor lightgbm`).

## D20. Window scaled with the cache, normalized features, settings stored with the model (M3 follow-up)

**Why:** with a fixed 10k-key window and raw ages, the pipeline lost to LRU on Twitter at 512 KiB
and to W-TinyLFU on cloudPhysicsIO at 64 and 256 MiB (docs/m3-results.md, step 9). A window of
10k keys is 8× more of the cache's contents at 512 KiB than at 4 MiB, and an age of 5,000
requests means "old" in a small cache and "recent" in a large one.

**Decision.**
- `FeatureConfig.history_ratio`: the ghost window holds at most `ratio × cached objects` keys,
  recomputed as the cache fills. (Capacity is in bytes, the window counts keys, so "2 ×
  capacity" is read as 2 × the number of cached objects.) The fixed `history` remains.
- `FeatureConfig.normalize`: age and gaps are divided by the number of cached objects. Normalized
  columns are named `age_norm`, `delta_i_norm`, so a model trained on one kind **fails at load**
  with the other: the policy takes the kind from the model's feature names.
- **Settings stored with the model:** `lcache-export` writes `<csv>.meta` (history,
  history_ratio, normalize), `train.py` requires all inputs to agree and writes `<model>.meta`,
  and `cachesim` / `lcache-export --model` apply it; an explicit flag that contradicts it is an
  error. This closes the train/serve gap left open in D17.

**Rejected:** normalizing by capacity in bytes (object sizes vary by 4 orders of magnitude on
cloudPhysicsIO, so bytes don't say how many items the cache holds); scaling the decayed-counter
half-lives too (not requested; can be tested the same way).

## D21. Defensive fallback: shadow W-TinyLFU, circuit breaker, time budget (M4)

`FallbackPolicy` ("guarded") wraps the learned policy.

- **Both policies track the real cache.** The learned policy and a W-TinyLFU both receive every
  hit and insert; whichever is active picks the victim and the other is told via `on_remove`.
  Switching is therefore instant, and the inactive policy's state is always valid. **Rejected:**
  rebuilding the fallback's state on demand (a burst of misses right when the model is failing).
- **Counterfactual = shadow W-TinyLFU.** A third, independent W-TinyLFU runs on its own
  key -> size bookkeeping and sees every request. Real hits vs shadow hits over a rolling window
  (ring buffer, default 10,000 requests) measure "are we beating W-TinyLFU right now". It
  duplicates `sim::Cache`'s rules (~15 lines) because `lc_sim` depends on `lc_policy`.
- **Circuit breaker.** Learned → fallback when, over a window of learned-mode requests, real hits
  < shadow hits − margin × window. Fallback → learned after a cooldown (default 50,000 requests),
  which doubles (to 16× at most) each time a retry fails its first window. **Early check:** from
  window/10 requests after a switch, trip if behind by more than margin × n + 3√n; the hit
  difference of two policies over n requests varies by at most about 0.7√n, so this rarely trips
  on noise but stops a clearly bad model long before a full window (added after a test with a
  deliberately bad model recovered only 89.2% of W-TinyLFU's hits with full-window checks only).
  **Rejected:** a shadow *learned* cache while fallen back (it would pay inference cost exactly
  when the latency guard says not to).
- **Time budget.** Each learned eviction is timed (`steady_clock`, injectable for tests);
  `max_breaches` evictions over budget since the last switch (default 1) trip to W-TinyLFU. Inference
  isn't interruptible, so the slow eviction completes and the switch applies from the next one.
  **Rejected for now:** a deadline-aware `evict()` that scores candidates until time runs out
  (cheaper per breach, but more code inside the hot path).
- With a time budget, results depend on machine speed and load; without one they are
  deterministic.

**D20 outcome (docs/m4-results.md, section 1).** Chosen on dev: **window 4 × cached objects,
normalized features ("r4n")**, the best average over six trace/size configurations (+1.12 pp per
configuration). On test it helps where the pipeline was weakest on cloudPhysicsIO (+5.89 pp at
256 MiB) but loses 0.9–2.6 pp elsewhere; it does not fix Twitter 512 KiB, where the model is weak
even offline. Not a general fix: the fallback (D21) handles the remaining losses.

**D21 additions (docs/m4-results.md, section 2).**
- **Grace period after each switch to the learned policy (default 30k requests).** Without it the
  guard tripped 38–43 times on Twitter 1 MiB, where the model wins, because the cache inherited
  from W-TinyLFU must be reshaped first. Chosen on dev over 0 and 10k by total shortfall vs the
  better of the two policies.
- **Budget breaches counted in a window of the last 1,000 learned evictions** (`--breach-window`),
  not since the last switch: at a 0.03–0.05% jitter rate, a never-decaying count trips
  eventually whatever the threshold. The default remains "trip on the first breach".
- **Open problem:** the learned policy does badly on a W-TinyLFU-shaped cache (never in its
  training data), so retries can be costly (cloudPhysicsIO 256 MiB test: guarded 16.69% vs 29.26%
  learned alone). Candidate fix: training data recorded from W-TinyLFU-shaped caches.

## D22. Server architecture: locked shards, hashed keys, standalone Asio (M5)

Full design: `docs/m5-design.md`.

- **Threads:** N I/O threads, each with its own `asio::io_context`; connections are assigned
  round-robin and stay on one thread (no strands). **Shards:** S ≫ N (default 64), each an
  `alignas(64)` struct with a `std::mutex`, one policy instance and its own predictor; any
  thread may lock any shard. **Rejected:** shared-nothing thread-owned shards (with random keys
  (N−1)/N of requests would hop threads, plus queues and reply reordering under pipelining);
  one shared `io_context` (strands, handler migration).
- **Plain mutex, not `shared_mutex`:** a GET hit calls `on_hit()`, so every op writes policy state.
  The lock covers only the in-memory op; GET copies the value out under it. **Rejected for
  v1:** `shared_ptr` values (atomic refcount per hit).
- **Policy key = 64-bit hash of the key bytes**, so a key evicted and SET again keeps its
  `FeatureTracker` history (D17, D20). On a collision within a shard the newer string replaces
  the older entry (legal for a cache); GET compares full strings. **Rejected:** per-shard counter
  ids (collision-free, but re-inserted keys lose their history).
  *Implementation:* one `unordered_map<Key, Entry{key, value}>` per shard instead of the two maps
  in the design: at most one key per id, so the id alone finds a victim's entry and GET compares
  `Entry::key`. The hash is FNV-1a plus the splitmix64 finalizer; the shard is its top bits.
- **Cache-aside semantics:** GET misses don't insert; SET inserts, then evicts until within the
  shard's capacity (D2). Policy virtual time (D11) thus counts hits and SETs only, a train/serve
  shift that the trace-replay check measures.
- **Standalone Asio (header-only, FetchContent), not Boost.Asio:** same library and API, without a
  Boost dependency.
- **RESP parser has no Asio dependency** (incremental, zero-copy `string_view` arguments), so it is
  unit-tested in isolation, including every split point of every input.
- **Connection loop:** strictly read → run every complete command → one `async_write` → repeat,
  so one async op is outstanding and a client that stops reading stops being read. A batch
  stops once 1 MiB of replies is pending (pipelined GETs of big values can't grow the buffer
  without bound) and resumes after the write. Each connection is started on its own
  `io_context` (posted from the accept handler), so only that thread ever touches it.
  **Rejected:** a write queue that keeps reading while writing (more throughput under deep
  pipelining, but unbounded memory unless a second limit is added).
- **Tool compatibility:** `COMMAND` → empty array; `CONFIG GET save|appendonly` → `""` / `"no"`,
  which is what `redis-benchmark` reads at startup.

## D23. Capacity split evenly across shards (M5)

- **Each of the S shards gets capacity / S bytes** and evicts on its own, with its own policy
  instance. A key's hash spreads keys evenly, so each shard sees a statistically similar
  sub-stream. **Rejected:** one global byte budget (a shared atomic counter, plus an eviction
  that must pick a shard to evict from: either a global policy, which brings back a global lock,
  or a heuristic such as "evict in the fullest shard", which is not the policy being measured).
- **Consequence, measured (docs/m5-results.md, section 3):** an object larger than one shard
  can never be stored (`too_large`): at 1 MiB and S = 64, 4,203 SETs were refused, which acts as
  a size-based admission filter (object hit rate up, byte hit rate down). At 16 MiB, sharding
  cost LRU 0.04 pp and guarded 0.2–0.5 pp. **Rule:** size the cache so one shard can hold the
  largest objects that matter. Routing large objects separately is deferred.

## D24. Checking the server against the simulator: same requests, the server's key ids (M5)

- `lcache-replay` replays a trace over RESP as a cache-aside client (GET; on a miss, SET with a
  value of size − key length, so the server charges exactly the simulator's size). `--mode
  simulate` runs `sim::Cache` on the same requests **with each object id replaced by the
  server's hash of its key**. With S = 1 the two agree hit for hit for LRU, W-TinyLFU and
  guarded (docs/m5-results.md, section 2).
- **Rejected:** comparing with cachesim on the trace's own ids. Only LRU matches that way,
  because W-TinyLFU's sketch hashes key values: relabeling the same 2M requests 12 ways moves
  W-TinyLFU's hit rate by up to 10 pp at 16 MiB (section 4).
- **W-TinyLFU is left unchanged** pending review of that sensitivity: it is a core policy, and the
  M2–M4 W-TinyLFU baselines carry that much uncertainty at some sizes until it is explained.

## D25. Benchmark method for the server (M5)

- **Client:** `redis-benchmark` 7.2.14 built from a SHA-256-pinned source tarball (the Ubuntu
  6.0.16 client reports latency only in whole-millisecond buckets; 7.x reports p50/p99/p99.9).
  **Baseline server:** the distro's `redis-server` 6.0.16, `maxmemory` + `allkeys-lru`, no
  persistence. Both servers get 32 MiB, below the working set, so eviction runs throughout.
- **Isolation:** a fresh server for every configuration; `taskset`: server on CPUs 0–7, client
  on 8–15. One run per configuration (no variance estimate yet).
- **Reported:** ops/s and p50/p99/p99.9 per request. Hit rates from `redis-benchmark` are
  not a policy comparison (uniform random keys), and Redis's are lower because its `maxmemory`
  includes per-key overhead.
- **Known limit, found in the data:** `redis-benchmark` notices that a run is done on a 250 ms
  tick, so with 2M requests any rate above ~400k ops/s is a lower bound that can be up to 33%
  low. **Fix for the next run:** scale `-n` so that every run lasts ≥ 10 s. Until then the
  pipelined comparisons are given as tick bins, not ratios.
- **Not changed:** the guard's default time budget stays off in `lcache-server`. At 10 and
  25 µs every shard tripped on its first learned eviction (the server measured about 70 µs per
  learned eviction with 64 candidates), so the right budget or candidate count is chosen after
  profiling (docs/m5-results.md, section 6).

## D26. Simulation API in C++, exact bucketed series, one simulation at a time (M6)

Full design: `docs/m6-design.md`.

- **C++ API (cpp-httplib + nlohmann/json) linking `lc_sim` in-process.** **Rejected:** FastAPI
  around `cachesim` (CLAUDE.md keeps Python to training and plotting; it would also parse CSV
  from a subprocess); HTTP by hand on the server's Asio (2–3× the code for no measured gain at
  a few requests per minute).
- **Handlers without HTTP:** `lc_api` maps JSON to JSON and throws `ApiError{status}`; only
  `main.cpp` knows httplib, so validation and simulation are unit-tested without sockets.
- **Downsampling = exact per-interval counts from the simulator** (`SimOptions::series_interval`),
  interval = ceil(N / points). **Rejected:** sampling points or LTTB, which approximate a signal
  that can be aggregated exactly.
- **Blocking request, capped, one at a time (429 when busy), up to 8 runs in parallel.** Measured
  at 1M requests: 0.12 s (LRU) to 6.2 s (learned) per run; Belady needs 530 MB at 10M, so
  concurrency is capped. **Rejected for now:** a job queue with polling.
- **Traces are named, never pathed:** the client sends a name that must appear in the server's
  listing of `--data-dir`.

## D27. Eviction window: object slots, a recording wrapper, stable slots (M6)

Full design: `docs/m6-part2-design.md`.

- **Capacity in objects (`ignore_size`), 2–16 slots, 10–100 requests.** A slot is one box on
  screen. Measured on 100 requests with 8 slots: LRU 11 hits vs Belady 21 (Twitter), 23 vs 35
  (cloudPhysicsIO), so a window shows real differences. **Rejected:** byte capacity (thousands
  of objects at any real size). Cost: policies see size 1, which the learned model never saw.
- **`RecordingPolicy` wraps the real policy** and records inserts and victims, so `sim::Cache`
  and the policies are untouched. **Rejected:** returning victims from `Cache::access`.
- **Stable slots, computed on the server:** a victim frees its slot, a newcomer takes the lowest
  free one, so boxes that did not change do not move. Server-side, so it is unit-tested and the
  UI only draws snapshots (≤ 100 × 16 × 4 entries).

## D28. Containers: two services behind one origin, traces mounted (M6)

- **`api` (C++ binary on Ubuntu 22.04) and `web` (static files on nginx), nginx forwarding
  `/api/`**, so the browser sees one origin, as with the Vite proxy in development. **Rejected:**
  `lcache-api` serving the static files (one container, but it mixes the dashboard's build into
  the C++ image).
- **Traces are mounted read-only from `./data`,** never baked into an image (the Twitter prefix is
  240 MB and the repo keeps traces out of git).
- **LightGBM off in the image:** the learned policy uses the compiled evaluator (D19).
- **Ports bound to 127.0.0.1** (3000 dashboard, 8000 API): nothing is exposed on the network.
