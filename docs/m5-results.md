# Milestone 5 results: sharded RESP server

Measured 2026-10-13, release build, i7-12700H under WSL2 (20 vCPUs). Design: `docs/m5-design.md`;
decisions D22–D25. Raw data: `bench/results/m5/`. Every number below comes from one run per
configuration; no repetitions, so run-to-run variance is not measured.

## 1. What was built

`lcache-server` speaks enough RESP2 (GET, SET, DEL, PING, QUIT, COMMAND, CONFIG GET) for
`redis-cli` and `redis-benchmark`. N I/O threads each run their own `asio::io_context`;
connections are assigned round-robin and stay on one thread. The store has S shards (default 64),
each a mutex, a hash map of values and **its own instance of the eviction policy** (`lru`,
`wtinylfu`, `guarded`, ...), unchanged from M1–M4. A key's 64-bit hash is both its policy key
(so a re-inserted key keeps its learned-feature history) and, by its top bits, its shard.

Correctness and thread safety (details in the commits):

| check | result |
|---|---|
| Unit + loopback tests (parser, store, commands, network) | pass in Debug, ASan+UBSan, TSan |
| Stress: 8 threads × 20k random ops, 7 policies incl. learned/guarded, capacity checked throughout | pass; TSan clean |
| Same test with the lock removed from `Shard::get` | TSan reports races (the test can catch them) |
| TSan `lcache-server` under `redis-benchmark -c 50 --threads 4`, ±`-P 16` (`tools/m5_tsan_server.sh`) | no reports |
| Full suite | 306/306 in all three builds |

## 2. The server reproduces the simulator (S = 1)

`lcache-replay` replays a trace over RESP with cache-aside semantics: GET, and on a miss SET with
a value of (size − key length) bytes, so the server charges exactly the simulator's size. Its
`--mode simulate` runs the simulator core (`sim::Cache`) on the same requests with each object id
replaced by the server's hash of its key. Script: `tools/m5_replay.sh`; data:
`bench/results/m5/replay.csv`. First 2M requests of Twitter cluster26 (165,835 objects, 28.5 MiB;
no object changes size and none is smaller than its key, so every request is representable).

**Hits out of 2,000,000:**

| capacity | policy | cachesim (trace ids) | simulator (server's ids) | server, 1 shard |
|---|---|---|---|---|
| 1 MiB | lru | 1,003,252 | 1,003,252 | **1,003,252** |
| 1 MiB | wtinylfu | 1,085,048 | 1,073,023 | **1,073,023** |
| 1 MiB | guarded | 1,034,496 | 1,043,510 | **1,043,510** |
| 16 MiB | lru | 1,834,156 | 1,834,156 | **1,834,156** |
| 16 MiB | wtinylfu | 1,739,788 | 1,641,621 | **1,641,621** |
| 16 MiB | guarded | 1,831,843 | 1,832,801 | **1,832,801** |

- **The 1-shard server matches the simulator hit for hit for every policy** when both see the same
  key ids: protocol, store, byte accounting and the cache-aside flow add no difference.
- **Against cachesim with the trace's own ids, only LRU matches exactly.** LRU never looks at key
  values; W-TinyLFU's frequency sketch hashes them, and guarded contains W-TinyLFU (fallback and
  shadow). Section 4 shows that this difference is much larger than it should be.
- The guarded model was trained on Twitter requests 0–5M (M4), so these 2M requests are training
  data for it: fine for an equivalence check, not a fair measure of the model.

## 3. Cost of sharding (S = 8, 64)

Same replay against `--shards 8` and `--shards 64` (each shard gets capacity / S).

**Hit rate % (byte hit rate %), and SETs refused because the object exceeds a shard:**

| capacity | policy | S = 1 | S = 8 | S = 64 |
|---|---|---|---|---|
| 1 MiB | lru | 50.16 (40.82) | 50.51 (34.07), 600 refused | 63.55 (35.02), 4,203 refused |
| 1 MiB | wtinylfu | 53.65 (40.87) | 56.97 (33.73), 600 refused | 64.65 (30.72), 4,203 refused |
| 1 MiB | guarded | 52.18 (45.77) | 54.74 (37.43), 600 refused | 61.74 (35.42), 4,203 refused |
| 16 MiB | lru | 91.708 (95.72) | 91.708 (95.72) | 91.671 (95.45) |
| 16 MiB | wtinylfu | 82.08 (92.41) | 88.73 (94.60) | 90.36 (94.69) |
| 16 MiB | guarded | 91.64 (95.68) | 91.42 (95.50) | 91.18 (95.16) |

- **At 16 MiB sharding is almost free for LRU** (−0.04 pp at 64 shards, 256 KiB each): a hash
  partition of the keys gives each shard a statistically similar stream, and LRU on a partition
  approximates LRU on the whole.
- **Guarded loses 0.2 pp (S = 8) and 0.5 pp (S = 64).** Each shard's model sees 1/S of the traffic
  in a cache 1/S the size it was trained for; normalized features (D20) don't fully cover that,
  and each shard's guard judges itself on 1/S of the requests.
- **At 1 MiB, sharding raises the object hit rate and lowers the byte hit rate.** With 64 shards a
  shard holds 16 KiB, so objects over 16 KiB can't be cached at all (4,203 SETs refused). That
  acts as a size-based admission filter: small objects get the room, so more requests hit, but
  fewer bytes do. It is not a better policy, and it means **a sharded cache must be sized so a
  shard can hold the largest objects that matter** (or route large objects separately).
- **The W-TinyLFU rows can't be read as a sharding effect** (82.1 → 88.7 → 90.4% at 16 MiB):
  section 4.

## 4. Finding: W-TinyLFU depends on how keys are numbered

The cachesim/simulator gap for W-TinyLFU in section 2 comes only from key ids. To measure it,
`tools/m5_wtinylfu_labeling.sh` rewrites the same 2M requests with the ids relabeled 12 ways
(identity, the server's hash, a multiplicative hash, 9 random permutations) and runs cachesim.
The request sequence is identical; only which keys collide in the sketch changes. Data:
`bench/results/m5/wtinylfu_labeling.csv`.

| capacity | LRU (all 12) | W-TinyLFU, min–max over 12 labelings |
|---|---|---|
| 1 MiB | 50.16 | 53.50–54.55 |
| 4 MiB | 88.77 | 78.73–81.17 |
| 8 MiB | 91.67 | 83.62–84.98 |
| 16 MiB | 91.71 | **80.43–90.54**, in two clusters (7 near 80–83, 5 near 87–90.5) |

- LRU is exactly invariant, as it must be. W-TinyLFU should be nearly invariant (the collision
  *rate* doesn't depend on the labeling), yet at 16 MiB it varies by 10 pp, bimodally. That looks
  like the cache settling into one of two regimes depending on early admission decisions; a
  plausible mechanism is the admission duel on 4-bit sketch counts with ties rejecting the
  candidate, where one collision can flip a decision and the cache contents feed back into later
  ones. **Not root-caused**; W-TinyLFU is a core policy, so it is left unchanged pending review.
- Consequences: (1) single-run W-TinyLFU numbers at some sizes carry several points of
  uncertainty, including the M2–M4 baselines; (2) the "sharding helps W-TinyLFU" rows above are
  likely this effect (each shard is a different sub-stream); (3) on this prefix W-TinyLFU trails
  LRU in every labeling at 4–16 MiB: by 7.6–10.0 pp at 4 MiB, 6.7–8.0 pp at 8 MiB and
  1.2–11.3 pp at 16 MiB, which is itself worth explaining.

## 5. Throughput and latency vs `redis-server`

Script: `tools/m5_bench.sh`; data: `bench/results/m5/bench.csv` (240 rows: 15 server
configurations × 8 client settings × SET and GET), raw output in `bench/results/m5/raw/`. Each
configuration runs on a freshly started server; server pinned to CPUs 0–7, client to CPUs 8–15.
Client: `redis-benchmark` 7.2.14 (built by `tools/m5_fetch_redis_benchmark.sh` for HdrHistogram
percentiles), `-t set,get -n 2000000 -r 1000000 --threads 4`, with clients {50, 200} ×
pipeline {1, 16} × value {32, 1024} B. Servers: `lcache-server` with N ∈ {1, 2, 4, 8} I/O
threads, 64 shards, lru / wtinylfu / guarded (`models/twitter26_1MiB_h10k_32x31.txt`, 64
candidates); `redis-server` 6.0.16 with `maxmemory 32mb`, `allkeys-lru`, no persistence. Both get
32 MiB, below the working set, so every run evicts. Decisions: D25.

**Read these numbers with three caveats:**
- **Throughput above ~400k ops/s is coarse.** `redis-benchmark` notices that the run is done on
  a 250 ms tick: every run above 400k ops/s took 0.75x, 1.00x, 1.25x, ... s, which is why values
  like 1,996,008 (2M / 1.002 s) and 2,663,116 (2M / 0.751 s) recur. The reported rate is a lower
  bound and can be up to one tick low (up to 33% at 0.75 s). Values marked † took under 5 s, so
  their error can exceed 5%. Latency percentiles come from per-request histograms and are not
  affected.
- **Hit rates here say nothing about policies.** Keys are uniform random, so every lcache policy
  gets the same GET hit rate (69.85–69.96% at 32 B, 3.20–3.26% at 1024 B). Redis gets less
  (14.3–21.5% and 1.5–2.2%) because its `maxmemory` counts its own per-key overhead while
  lcache charges key + value bytes; Redis therefore holds fewer keys and answers more GETs
  with nil.
- **Sporadic stalls.** 62 of 240 rows have a max latency over 500 ms (563–1,587 ms), spread over
  every configuration including Redis; in 9 rows the stall reaches p99.9, in none p99. This
  looks like scheduling pauses of the WSL2 VM (not attributed). With one run per configuration,
  p99.9 and max are unreliable; p99 is the tail figure to use.

**Throughput, thousands of ops/s** (N = I/O threads; † = run under 5 s, a lower bound):

| clients, pipeline, value | test | redis | lru N=1 | lru N=8 | wtinylfu N=8 | guarded N=8 | guarded N=8, 25 µs |
|---|---|---|---|---|---|---|---|
| 50, 1, 32 B | GET | 109 | 113 | 244 | 258 | 228 | 243 |
| 50, 1, 32 B | SET | 90 | 84 | 258 | 212 | 160 | 235 |
| 50, 1, 1024 B | GET | 111 | 109 | 276 | 258 | 236 | 276 |
| 50, 1, 1024 B | SET | 97 | 103 | 258 | 250 | 58 | 216 |
| 200, 1, 32 B | GET | 138 | 80 | 257 | 285 | 296 | 307 |
| 200, 1, 32 B | SET | 118 | 117 | 285 | 257 | 182 | 275 |
| 200, 1, 1024 B | GET | 145 | 153 | 285 | 275 | 251 | 296 |
| 200, 1, 1024 B | SET | 117 | 154 | 296 | 285 | 57 | 222 |
| 50, 16, 32 B | GET | 887† | 499† | 1,996† | 1,329† | 1,330† | 1,596† |
| 50, 16, 32 B | SET | 614† | 347 | 1,330† | 886† | 307 | 799† |
| 50, 16, 1024 B | GET | 999† | 799† | 2,663† | 1,994† | 1,996† | 1,996† |
| 50, 16, 1024 B | SET | 400 | 352 | 1,331† | 785† | 73 | 666† |
| 200, 16, 32 B | GET | 950† | 571† | 1,992† | 1,994† | 1,329† | 1,595† |
| 200, 16, 32 B | SET | 614† | 470† | 1,594† | 736† | 307 | 798† |
| 200, 16, 1024 B | GET | 1,329† | 1,141† | 2,653† | 2,653† | 2,656† | 2,656† |
| 200, 16, 1024 B | SET | 531† | 420† | 1,591† | 1,328† | 77 | 726† |

**GET latency, p50 / p99 / p99.9 in ms:**

| clients, pipeline, value | redis | lru N=1 | lru N=8 | guarded N=8 |
|---|---|---|---|---|
| 50, 1, 32 B | 0.431 / 0.823 / 1.223 | 0.399 / 0.735 / 1.007 | 0.103 / 0.487 / 0.759 | 0.119 / 0.599 / 0.927 |
| 50, 1, 1024 B | 0.423 / 0.823 / 1.167 | 0.391 / 0.991 / 1.967 | 0.103 / 0.439 / 0.711 | 0.111 / 0.447 / 0.735 |
| 200, 1, 32 B | 1.351 / 2.767 / 3.799 | 1.423 / 4.639 / 5.335 | 0.335 / 1.239 / 2.087 | 0.343 / 1.383 / 2.191 |
| 200, 1, 1024 B | 1.287 / 2.583 / 3.199 | 1.119 / 2.415 / 2.799 | 0.343 / 1.279 / 2.079 | 0.343 / 1.047 / 2.015 |
| 50, 16, 32 B | 0.767 / 1.503 / 1.903 | 1.463 / 3.111 / 4.543 | 0.247 / 0.767 / 1.151 | 0.447 / 1.159 / 1.551 |
| 50, 16, 1024 B | 0.687 / 1.295 / 1.663 | 0.927 / 1.247 / 1.495 | 0.151 / 0.623 / 0.855 | 0.215 / 0.679 / 0.879 |
| 200, 16, 32 B | 2.167 / 3.887 / 603.647 | 5.383 / 6.455 / 10.311 | 0.871 / 2.143 / 2.807 | 1.855 / 4.047 / 5.551 |
| 200, 16, 1024 B | 1.823 / 3.183 / 5.151 | 2.063 / 4.591 / 6.895 | 0.527 / 1.999 / 2.823 | 0.759 / 2.151 / 3.143 |

- **One thread each, no pipelining: lcache ≈ Redis.** LRU at N = 1 is within 8% of Redis in 6 of
  8 cells. The exceptions are single runs: GET at 200 clients / 32 B is 42% lower (that run also
  has a 710 ms stall) and SET at 200 clients / 1024 B is 32% higher.
- **One thread each, pipelined: Redis is faster.** GET at 50 clients / 32 B: Redis finished in
  2.25 s and lcache in 4.01 s, a gap much larger than one 250 ms tick. Redis does more work per
  thread when pipelining; why has not been profiled.
- **With 8 threads and no pipelining: 1.9–2.5× Redis for GET, 2.4–2.9× for SET**, with GET p99
  0.44–1.28 ms against Redis's 0.82–2.77 ms. Redis 6 runs commands on one thread; the comparison
  is lcache's best configuration against Redis's only one, as planned in the design.
  N = 4 and N = 8 are close (N = 4 is ahead in some cells), so beyond 4 threads the limit is
  elsewhere: the 4-thread client, or the server's 8 vCPUs if they are hyperthread pairs. Not
  separated.
- **With 8 threads and pipelining**, lcache LRU reaches the top tick bins (2.0–2.7M GETs/s, lower
  bounds) where Redis reports 0.89–1.33M. GET p99 is 0.62–2.14 ms against Redis's 1.30–3.89 ms.
  The ratio needs a longer run (follow-up 1).

## 6. The learned policy in the server, and the circuit breakers

GET hits only update policy state (`on_hit`), so guarded GET throughput is in LRU's range. The
model runs on evictions, i.e. inside SETs. At 1024 B nearly every SET evicts (1.90M evictions per
2M SETs); at 32 B about one in eight does (240k).

**SET throughput, thousands of ops/s, and SET p99 in ms, by I/O threads N:**

| setting | policy | N = 1 | N = 2 | N = 4 | N = 8 |
|---|---|---|---|---|---|
| 50 clients, P = 1, 1024 B | lru | 103 (0.74) | 195 (0.59) | 275 (0.60) | 258 (0.53) |
| | wtinylfu | 122 (0.61) | 180 (0.51) | 285 (0.58) | 250 (0.55) |
| | guarded | 13 (6.16) | 23 (3.54) | 40 (1.81) | 58 (1.65) |
| | guarded, budget 10 / 25 µs | | | | 205 (0.66) / 216 (0.62) |
| 200 clients, P = 16, 1024 B | lru | 420† (9.6) | 614† (8.9) | 1,329† (3.5) | 1,591† (3.2) |
| | wtinylfu | 380 (10.2) | 799† (4.5) | 1,332† (3.8) | 1,328† (4.2) |
| | guarded | 17 (293.9) | 34 (133.4) | 52 (85.8) | 77 (53.2) |
| | guarded, budget 10 / 25 µs | | | | 665† (9.0) / 726† (8.7) |

- **A learned eviction costs about 70 µs in the server.** At N = 1 (50 clients, P = 1, 1024 B) a
  guarded SET takes 1/13,018 − 1/122,264 s = 68.6 µs longer than a W-TinyLFU SET, with 0.95
  evictions per SET. That is more than twice M4's `eviction_bench` median for the same model and
  64 candidates (30–33 µs, a different run). Not profiled yet (follow-up 2).
- **Evictions parallelise across shards:** guarded SETs go from 13k to 58k/s with 1 to 8 threads
  (4.4×), but that is still 4.5× below LRU. Pipelined SETs queue behind evictions: SET p99 is
  53–294 ms against 3–10 ms for LRU.
- **Hit-rate breaker: no trips in any of the 32 guarded runs without a budget**, and all 64
  shards were still in learned mode at shutdown. With uniform keys there is nothing for any
  policy to win (hit rates within 0.1 pp), so this shows only that the breaker does not trip
  falsely under concurrent load. Its behaviour on real traces is in `docs/m4-results.md`.
- **Latency breaker: with `--budget-us 10` and `--budget-us 25`, every one of the 16 runs shows
  64 learned evictions, 64 budget breaches, 64 latency trips, 0 retries and 0/64 shards in learned
  mode at shutdown.** Each shard's first learned eviction exceeded the budget and the shard
  switched to W-TinyLFU at once (D21 default: trip on the first breach). It never retried: a
  shard's policy sees SETs and GET hits only, about 32–53k requests per shard over a whole run,
  which is less than the 50,000-request cooldown after the switch.
- **What that buys:** SET throughput returns to the W-TinyLFU level (58k → 205–216k/s at 50
  clients, P = 1; 77k → at least 665–726k/s at 200 clients, P = 16) and SET p99 falls from 1.65
  to 0.62–0.66 ms and from 53.2 to 8.7–9.0 ms. So the breaker works as a latency guard, but
  **at 10 and 25 µs it switches the model off for good**: those runs measure "guarded with the
  model disabled", not the learned policy. A useful budget must sit above the per-eviction cost
  measured here, or the model must get cheaper (M4: about 8 µs with 16 candidates). Which one is
  an open choice.

## 7. Milestone 5 status

Done criterion: *ops/sec and p99 latency measured, TSan clean.* **Met:** sections 1 and 5–6.
`redis-benchmark` drove all 240 runs, and `redis-cli` checked readiness before each one.

Follow-ups, not done here:
1. Rerun the pipelined settings with more requests (≥10 s per run) to get past the 250 ms tick,
   and repeat each configuration ≥3 times to measure variance.
2. Profile the learned eviction inside the server (about 70 µs vs 30–33 µs in `eviction_bench`).
3. Choose the server's budget / candidate count from that profile (section 6).
4. Root-cause W-TinyLFU's sensitivity to key labeling (section 4); core policy, pending review.
