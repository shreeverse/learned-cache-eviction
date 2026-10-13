# M6 design (part 2): eviction window, containers

Approved 2026-10-13. Part 1 is `m6-design.md`. Decisions are logged in `decisions.md` (D27, D28).

**Goal.** Animate what each policy keeps and evicts over a short window of a real trace; run the
whole stack with `docker compose up`; test the HTTP layer; README screenshots.

## 0. Measurement behind the defaults

`cachesim --ignore-size -n 100`, 8 objects: Twitter LRU 11 hits, Belady 21; cloudPhysicsIO LRU
23, Belady 35. A 100-request window with a few slots shows real hits, misses and evictions.

## 1. `POST /api/visualize-window`

- **Request:** `{trace, policies (1–4), slots (2–16, default 8), start (0–1,000,000, default 0),
  length (10–100, default 60)}`. `start + length` must fit in the trace.
- **Capacity in objects** (`ignore_size`, as `cachesim --ignore-size`): every request has size 1,
  so a slot is one object. **Rejected:** byte capacity with variable-size boxes (at any real
  size the cache holds thousands of objects). Policies see size 1, so the learned model gets an
  out-of-distribution size feature; the UI says so.
- **Replay:** requests `[0, start)` warm the cache silently, then `[start, start + length)` are
  recorded. Belady's future (annotation) ends at the window's end, as with `max_requests`.
- **`RecordingPolicy`** (`api/visualize.h`): an `EvictionPolicy` that forwards every call to the
  real policy and records `on_insert` and the keys `evict()` returns. `sim::Cache` and the
  policies are unchanged. **Rejected:** returning victims from `Cache::access` (changes the
  simulator core for a UI).
- **Stable slots:** a victim frees its slot; a newcomer takes the lowest free slot. A newcomer
  that is itself the victim (D2) never gets a slot.
- **Labels:** each key gets a short label (A … Z, AA, …) in order of first appearance: the cache
  contents at `start` (slot order, per policy in request order), then the window's requests.
  `keys` maps label → id.
- **Response:**

```
{trace, start, slots, length,
 requests: ["A", "B", ...],                 // the window, as labels
 keys: {"A": "1234", ...},                  // label -> key id (decimal string: ids are u64)
 policies: [{policy, hits,
             initial: ["C", null, ...],     // slots at `start`
             steps: [{hit, slot,            // slot of the accessed key after the step, or null
                      evicted: ["D"],       // victims in eviction order
                      slots: [...]}]}]}     // slots after the step
```

- Shares the "one heavy request at a time" lock with simulate (429 when busy).

## 2. Web: tabs and `EvictionVisualizer`

- Tabs "Hit rates" (part 1) and "Eviction window". The window tab has its own form (trace, up
  to 4 policies, slots, start, length) because its settings differ from the simulation's.
- A strip of the window's keys with a cursor; one lane of slot boxes per policy; the accessed
  key outlined with ✓ hit / ✗ miss (icon + word); evicted keys struck through in a per-lane
  list; hits so far per lane.
- Controls: play / pause, step back / forward, reset, speed (0.5–4 steps/s), scrubber. Playback is
  a `setInterval` inside a `useEffect` with cleanup.

## 3. HTTP integration tests

Route setup moves from `api/main.cpp` to `api/routes.{h,cpp}` (`lc_api_http`):
`register_routes(server, api, busy_mutex)`. Tests bind the real server to a free port and call it
with `httplib::Client`: status, content type and JSON body for every route, 404, bad JSON, 400,
and 429 (the test holds the mutex).

## 4. Containers

| piece | build stage | runtime stage |
|---|---|---|
| `api/Dockerfile` (context: repo root) | `ubuntu:jammy-20260901.2`, GCC 11; only `lcache-api`; tests, benchmarks and LightGBM off | same base; binary + `models/`; non-root; `0.0.0.0:8000` |
| `web/Dockerfile` | `node:22.23.3-alpine3.24`, `npm ci && npm run build` | `nginx:1.30.5-alpine`: `dist/`, `/api/` → `api:8000` |

- `docker-compose.yml`: `api` on `127.0.0.1:8000`, `web` on `127.0.0.1:3000` (localhost only);
  `./data` mounted read-only at `/app/data`. Traces are fetched with `tools/fetch_traces.sh`.
- LightGBM is off in the image: the learned policy uses the compiled evaluator (D19), so the
  image skips a multi-minute LightGBM build.
- nginx `proxy_read_timeout 300s`: a 10M-request learned simulation takes minutes.
- `.dockerignore` keeps `build/`, `data/` (1 GB), `node_modules/` and `.git/` out of the
  context.

## 5. Verification (2026-10-13)

- Tests: 341 pass under debug, ASan + UBSan and TSan (new: 9 visualizer, 7 HTTP integration).
- The window's hit counts equal `cachesim --ignore-size` on the same requests (cloudPhysicsIO,
  first 100 requests, 8 slots: LRU 23, Belady 35, learned 12), natively and in the container.
- `docker compose up --build` from scratch: 2 min 11 s to build both images (API 124 MB, web
  94 MB); the API turns healthy within seconds and the dashboard starts after it. Through
  nginx on :3000, Twitter simulate results equal the native build's (LRU 1 MiB 0.503035,
  W-TinyLFU 0.54811, Belady 0.608592).
- Dashboard in headless Chromium: tab switching keeps each tab's state; a fifth policy is
  disabled; stepping, playing (12 → 18 steps in 1.6 s at 4/s) and stopping at the end work; no
  console errors. Screenshots in `docs/img/` come from the Docker stack.
- Found in that pass: the ⏮ glyph is missing from some fonts (replaced by "Start"); showing a
  hidden tab restarts its 300 ms slot animations (harmless).
- The learned model is out of distribution at a few object slots (size 1, capacity 2–16):
  on cloudPhysicsIO's first 20,000 requests at 8 slots it gets 502 hits vs LRU's 1,281 and
  Belady's 2,588. `guarded` equals `learned` there because its 30,000-request grace period has
  not ended. The UI says the model is out of its depth here.
