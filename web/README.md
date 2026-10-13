# Dashboard

A small React app (plain JavaScript, Vite, Recharts) over `lcache-api`: pick a trace, policies
and cache sizes, run the simulator, and compare hit rates. Design: `docs/m6-design.md`.

## Run it

Needs Node.js 22 and a release build of `lcache-api` (see the top-level README).

```bash
# Terminal 1, from the repo root: the API on 127.0.0.1:8080, serving the traces in data/
build/release/api/lcache-api

# Terminal 2: the dashboard on http://localhost:5173
cd web
npm ci          # exact versions from package-lock.json
npm run dev
```

The Vite dev server forwards `/api/*` to `http://127.0.0.1:8080` (`vite.config.js`); set
`LCACHE_API=http://host:port` to point it elsewhere. If the page says it cannot reach the API,
terminal 1 is not running.

Other scripts: `npm run lint` (oxlint), `npm run build` (static files in `dist/`).

In Docker (`docker compose up --build` from the repo root), the `Dockerfile` here builds the
static files with Node and serves them with nginx on port 3000; `nginx.conf` forwards `/api/` to
the API container, so the code is the same in both setups.

## Code map

| file | role |
|---|---|
| `src/main.jsx` | entry point: renders `<App />` into `index.html` |
| `src/App.jsx` | loads traces and policies, the two tabs, runs simulations, holds loading / error state; `ResultsView` holds the view controls |
| `src/api.js` | `fetch` wrappers; turns `{"error": ...}` replies into thrown `Error`s |
| `src/format.js` | byte sizes, counts, percentages, log-spaced cache sizes |
| `src/colors.js` | fixed colour per policy (validated palette, light and dark) |
| `src/components/ControlPanel.jsx` | trace, policies, cache-size sliders, request count, Run |
| `src/components/HitRateChart.jsx` | hit rate vs cache size (log x axis) |
| `src/components/SeriesChart.jsx` | hit rate over the trace at one size (server-downsampled) |
| `src/components/ResultsTable.jsx` | every number behind the charts |
| `src/components/ChartTooltip.jsx` | hover tooltip shared by both charts |
| `src/components/EvictionVisualizer.jsx` | the Eviction window tab: its form, playback (timer in `useEffect`), request strip and one lane of slots per policy |

## Versions

Pinned exactly in `package.json`: React 19.2.8, Vite 8.3.1, Recharts 3.10.1, oxlint 1.85.0. The
lock file was created with `npm install --before=2026-09-29`, so every package in the tree had
been public for at least two weeks, except `source-map-js` 1.2.2, taken for a security fix
(`npm audit` reports 0 vulnerabilities).
