# Learned Cache Eviction Server

## Pitch
A cache server where an ML model predicts which items won't be needed soon, and evicts them first. It is benchmarked against classic policies (LRU, ARC, W-TinyLFU) and the offline optimum (Belady) on real production cache traces.

## Why this project exists
Flagship resume project for SDE roles at big tech. Theme across my three projects: "ML for Systems" — applying ML to core systems problems while building the systems myself. Every resume claim must be backed by a measured number from this repo.

## Tech stack and constraints
- C++20, CMake, Linux (WSL2/Ubuntu)
- GoogleTest for correctness, Google Benchmark for performance
- ASan, TSan and UBSan builds; all tests must pass under sanitizers
- clang-format and clang-tidy
- Python only for model training and plotting (pandas, LightGBM, matplotlib)
- Inference in C++ via LightGBM C API (or ONNX Runtime); no GPU
- Boost.Asio for the network server (phase 5)
- Docker for reproducible benchmark runs; GitHub Actions CI
- Frontend (small, secondary): React with plain JavaScript (Vite, JS template — no TypeScript), a charting library (e.g., Recharts), talking to a small JSON HTTP API over the simulator

## Architecture (modules)
- `trace/` — trace readers (Twitter, Wikipedia formats), sampling, a common request struct
- `policy/` — `EvictionPolicy` interface; LRU, LFU, ARC, W-TinyLFU, Belady (offline), Learned
- `sketch/` — count-min sketch, doorkeeper bloom filter
- `sim/` — trace-driven simulator: replays a trace against a policy at a given cache size, reports hit rate / byte hit rate
- `ml/` — feature extraction (inter-arrival deltas, frequency, size, age), training data export, C++ inference wrapper
- `server/` — sharded, thread-safe store + subset of Redis RESP protocol (GET/SET/DEL)
- `bench/` — benchmark runners, plots, results tables
- `tools/` — Python training and plotting scripts
- `api/` — small JSON HTTP endpoint that runs a simulation (trace, policies, cache size) and returns results
- `web/` — React (JavaScript) dashboard

## MVP milestones (build in order; each has a "done when")
1. **Simulator + LRU** — done when: LRU hit rate on a sample trace matches libCacheSim within 0.5%.
2. **Classic policies** — LFU, ARC, W-TinyLFU, Belady. Done when: unit tests cover each policy's eviction order on hand-built sequences; results table across 3 cache sizes.
3. **Learned policy (offline)** — features + LightGBM model predicting time to next access; evict the candidate with the furthest predicted next access from a sampled candidate set (Learning Relaxed Belady idea). Done when: hit-rate comparison graph vs W-TinyLFU and Belady.
4. **Defensive fallback** — strict per-eviction time budget; track rolling hit rate of learned vs shadow W-TinyLFU; fall back automatically when the model underperforms. Done when: a test with a deliberately bad model shows fallback kicking in.
5. **Network server** — sharded store, RESP subset, works with real `redis-cli` and `redis-benchmark`. Done when: ops/sec and p99 latency measured, TSan clean.
6. **Web dashboard (small)** — choose a trace, cache sizes and policies; show hit-rate vs cache-size charts and a results table; optional animated view of which keys each policy evicts on a short trace window. Must stay responsive with large result sets (downsample server-side, no UI freeze). Done when: the dashboard runs via `docker compose up` and a screenshot/GIF is in the README.
7. **Proof** — README with pitch, architecture diagram, graphs, design decisions and rejected trade-offs; `docker compose up` runs the benchmark; CI badge.

## Deferred (v2)
Online retraining, byte-hit-rate-aware eviction, multiple model types comparison, admission policies, replication.

## How to work with me
I am a student building this to learn and to defend it in interviews.
- Before writing code for a milestone, propose the design (interfaces, data structures, complexity) and wait for my OK.
- For core algorithms (policies, sketches, simulator core, learned eviction logic), write the interface and tests first; I may implement the logic myself — ask me which I want.
- Scaffolding (CMake, CI, Docker, benchmark plumbing, plotting scripts) can be written directly.
- Explain non-obvious choices briefly in comments or in the reply. Keep a `docs/decisions.md` log of design decisions and trade-offs.
- Never invent benchmark numbers; only report what was measured.
- Small commits, one logical change each.
- I am new to React and JavaScript. For frontend work, keep the code simple (function components, hooks, no heavy frameworks), and briefly explain React concepts (components, props, state, useEffect) the first time they appear.
