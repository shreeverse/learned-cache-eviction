# M6 design (part 1): simulation API and React dashboard

Approved 2026-10-13. Decisions are logged in `decisions.md` (D26).

**Goal.** Pick a trace, policies and cache sizes in a browser, run the simulator, and see hit rate
vs cache size, hit rate over time and a results table. The UI must stay responsive with large
traces, so the server sends only downsampled series. Part 2 adds `docker compose up` and the
README screenshot.

## 0. Constraints and measurements

- CLAUDE.md: Python only for training and plotting, so the API is C++ and calls
  `lcache::simulate()` in-process (no subprocess, no CSV parsing).
- Run time, release build, Twitter cluster26, 1 MiB, first 1M requests (`cachesim`, one run each):
  LRU 0.12 s, ARC 0.16 s, W-TinyLFU 0.16 s, Belady 0.43 s, learned (32 × 31, k = 16) 6.2 s.
  Full 10M: LRU 1.9 s; Belady 3.6 s and 530 MB peak (its annotated in-memory trace, D7).
  So a request can block until it is done if the request count is capped, and the number of
  concurrent runs must be capped for memory.

## 1. Simulator: per-interval counts

- `SimOptions::series_interval` (0 = off). When set, `SimResult::series` holds one
  `SeriesBucket{requests, hits, bytes_requested, bytes_hit}` per `series_interval` counted
  requests (after warmup); the last bucket may be partial. Buckets sum to the totals.
- **Downsampling is exact aggregation:** the API sets `interval = ceil(N / series_points)`, so a
  run of N counted requests yields at most `series_points` buckets, each the exact hit rate of
  its interval. **Rejected:** point sampling (each request is a 0/1 hit, so samples are noise);
  LTTB (picks representative points of a signal one cannot aggregate; here one can).

## 2. API (`api/`, binary `lcache-api`)

- **cpp-httplib 0.18.3** (single header, thread-per-connection pool) and **nlohmann/json
  3.11.3**, pinned by hash in `cmake/Deps.cmake`. Default bind `127.0.0.1:8080`.
- **`lc_api` library has no httplib dependency:** an `Api` class maps request JSON to response
  JSON and throws `ApiError{status, message}`; `main.cpp` only wires routes and turns
  exceptions into status codes. So handlers are tested without sockets.

| Endpoint | Returns |
|---|---|
| `GET /api/traces` | `[{name, format, bytes, requests}]` for `*.oracleGeneral.bin` and `*.csv` in `--data-dir` |
| `GET /api/policies` | `[{id, label, offline, available, note}]`: lru, lfu, arc, wtinylfu, belady, learned, guarded |
| `POST /api/simulate` | `{trace, requests, warmup, elapsed_ms, results: [...]}` |

- `requests` per trace: file size / 24 for oracleGeneral; counted by one reader pass for CSV.
  `.zst` files are not listed (no reader).
- **Simulate request:** `{trace, policies, cache_sizes, max_requests = 1'000'000, warmup = 0,
  series_points = 200}`. Limits: `max_requests` ≤ 10M, 1–7 policies, 1–12 sizes in
  [1 KiB, 1 TiB], `series_points` in [10, 1000], warmup < requests replayed.
- **Each result:** `{policy, cache_size, requests, hits, hit_rate, bytes_requested, bytes_hit,
  byte_hit_rate, elapsed_ms, series: {interval, hit_rate: [...], byte_hit_rate: [...]}}`.
  Ordered by policy (as requested), then cache size.
- **Trace by name only:** `trace` must equal a name from the current listing, so no path from
  the client reaches the filesystem (no `../`).
- **Learned / guarded** use `--model` (default `models/twitter26_1MiB_h10k_32x31.txt`), the
  settings in its `.meta` (D20) and k = 16 (models/README.md). If the model is missing they are
  listed with `available: false`. Their `note` says the model was trained on Twitter at 1 MiB.
- **Execution:** the (policy, size) runs are independent; up to `--workers` (default 8) threads
  take them from an atomic index, each with its own `TraceReader` (readers hold a file position)
  and a fresh policy, writing to its own result slot. **One simulation at a time:** a second
  `POST /api/simulate` while one runs gets 429. This bounds memory (8 Belady runs at 10M ≈ 4 GB)
  and CPU. **Rejected for now:** a job queue with polling (needed only if requests take
  minutes); a single-threaded loop (a 5 × 5 request would take 25× one run).
- Errors: 400 for invalid input (message says which field), 404 unknown route, 429 busy, 500
  for anything else, always `{"error": "..."}`.

## 3. Web (`web/`)

- Vite 8 + React 19 (JavaScript template, no TypeScript) + Recharts 3, linted with oxlint (what
  the official `create-vite` React template uses). *Changed during implementation:* the approved
  plan said Vite 5 / React 18 / Recharts 2, but Vite 5 is three majors behind and no longer
  maintained, so the current stable majors were pinned instead (versions in `web/README.md`).
  In development Vite proxies `/api` to `http://127.0.0.1:8080`, so no CORS headers are needed.
- `App` owns the state (selection, result, loading, error) and calls `api.js`.
- `ControlPanel`: trace `<select>`, policy checkboxes, cache sizes from two log-scale sliders
  (min, max) and a point count → log-spaced sizes shown as chips, max requests, Run (disabled
  while running).
- `HitRateChart`: hit rate (or byte hit rate) vs cache size, log x axis, one line per policy.
- `SeriesChart`: hit rate over requests for one chosen cache size, from the downsampled series.
- `ResultsTable`: policy, size, requests, hits, hit %, byte hit %, run time.
- Loading and error states. The previous result stays on screen, dimmed, while a new run is in
  progress. *Changed during implementation:* no `AbortController`. Run is disabled while a
  simulation runs because the server cannot cancel one (it would keep the lock and answer the
  next run with 429), so there is never a second fetch to abort.

## 4. Tests

- `sim_test`: buckets sum to totals, warmup excluded, partial last bucket, interval 0 → empty.
- `api_test`: trace listing on a temp dir; request validation (unknown trace, `../`, empty or
  unknown policies, too many sizes, bad types); `simulate` on a small CSV trace equals direct
  `simulate()` calls; series length ≤ `series_points`.
- ASan/UBSan and TSan presets (the worker pool). Frontend: `npm run lint` and `npm run build`.
- Manual: `curl` each endpoint against `data/`; the dashboard against the running API.

## 5. Verification (2026-10-13)

- Tests: 325 pass under debug, ASan + UBSan and TSan (new: `api_test` 13, `sim_test` 5).
- `lcache-api` against `cachesim` on Twitter, first 1M requests: identical hit rates for LRU
  (1 MiB: 0.503035, 64 MiB: 0.915976), W-TinyLFU (1 MiB: 0.54811) and Belady (1 MiB: 0.608592).
- One request with 7 policies × 4 sizes (28 runs, 1M requests each): 10.8 s wall time, 95 KB
  response (200 points per run). The slowest run, learned at 1 MiB (9.8 s), sets the wall time.
- Concurrency: three simultaneous `POST /api/simulate` gave one 200 and two 429; TSan clean.
  SIGINT shuts the server down cleanly (exit 0, no ASan leak report).
- Dashboard, driven by headless Chromium (Playwright) through the Vite proxy: load, run (5
  policies × 5 sizes on Twitter in about 7.5 s), tooltip, metric and size switches, light and
  dark mode, 390 px width without horizontal page scroll, API down (error banner, Run
  disabled), server busy (429 shown). No console errors or warnings.
- Found and fixed in that pass: the requests field had `step=100000` with `min=1`, so the
  browser's form validation rejected the default 1,000,000 and Run did nothing.
- The production bundle is 569 KB minified (mostly Recharts); Vite warns above 500 KB. Not
  split for now: it is a local tool loaded once.
