import { useEffect, useState } from 'react'
import { fetchPolicies, fetchTraces, runSimulation } from './api.js'
import { useChartTheme } from './colors.js'
import ControlPanel from './components/ControlPanel.jsx'
import EvictionVisualizer from './components/EvictionVisualizer.jsx'
import HitRateChart from './components/HitRateChart.jsx'
import ResultsTable from './components/ResultsTable.jsx'
import SeriesChart from './components/SeriesChart.jsx'
import { formatBytes, formatCount } from './format.js'

// App owns the data shared by several components (the server's lists, the last result, and
// the loading / error state) and passes it down as props ("lifting state up").
export default function App() {
  const [traces, setTraces] = useState([])
  const [policies, setPolicies] = useState([])
  const [loaded, setLoaded] = useState(false)
  const [loadError, setLoadError] = useState(null)
  const [tab, setTab] = useState('rates')
  const theme = useChartTheme()
  const [result, setResult] = useState(null)
  const [running, setRunning] = useState(false)
  const [runError, setRunError] = useState(null)

  // useEffect runs code after React has rendered, for work outside rendering such as network
  // requests. The empty dependency list [] means "only after the first render". The returned
  // cleanup function runs if the component goes away first; the `ignore` flag then drops the
  // late response. (In development, StrictMode deliberately mounts twice to exercise this.)
  useEffect(() => {
    let ignore = false
    Promise.all([fetchTraces(), fetchPolicies()])
      .then(([traceList, policyList]) => {
        if (ignore) return
        setTraces(traceList)
        setPolicies(policyList)
        setLoaded(true)
      })
      .catch((error) => {
        if (!ignore) setLoadError(error.message)
      })
    return () => {
      ignore = true
    }
  }, [])

  // The Run button is disabled while a simulation runs: the server cannot cancel one and
  // answers a second request with 429, so there is never more than one in flight.
  async function handleRun(request) {
    setRunning(true)
    setRunError(null)
    try {
      setResult(await runSimulation(request))
    } catch (error) {
      setRunError(error.message)
    } finally {
      setRunning(false)
    }
  }

  return (
    <div className="app">
      <header>
        <h1>Learned Cache Dashboard</h1>
        <p className="subtitle">
          Replay a production trace against eviction policies and compare hit rates.
        </p>
      </header>

      {loadError && <ErrorBanner message={loadError} />}
      {loaded && traces.length === 0 && (
        <ErrorBanner
          message={
            'No traces found in data/. Download them with tools/fetch_traces.sh ' +
            '(see the README), then reload.'
          }
        />
      )}

      <div className="tabs" role="tablist" aria-label="Views">
        {TABS.map((t) => (
          <button
            key={t.id}
            type="button"
            role="tab"
            id={`tab-${t.id}`}
            aria-selected={tab === t.id}
            aria-controls={`panel-${t.id}`}
            className={tab === t.id ? 'tab selected' : 'tab'}
            onClick={() => setTab(t.id)}
          >
            {t.label}
          </button>
        ))}
      </div>

      {/* Both panels stay mounted and the inactive one is only hidden, so switching tabs keeps
          each one's state (the last results, the loaded window). Rendering only the active
          one would unmount the other and throw its state away. */}
      <div role="tabpanel" id="panel-rates" aria-labelledby="tab-rates" hidden={tab !== 'rates'}>
        <div className="stack">
          <ControlPanel traces={traces} policies={policies} running={running} onRun={handleRun} />
          {runError && <ErrorBanner message={runError} />}
          {running && (
            <p className="status" role="status">
              Running simulation…
            </p>
          )}
          {result && (
            <ResultsView result={result} policies={policies} running={running} theme={theme} />
          )}
        </div>
      </div>
      <div role="tabpanel" id="panel-window" aria-labelledby="tab-window" hidden={tab !== 'window'}>
        <EvictionVisualizer traces={traces} policies={policies} theme={theme} />
      </div>
    </div>
  )
}

const TABS = [
  { id: 'rates', label: 'Hit rates' },
  { id: 'window', label: 'Eviction window' },
]

const METRICS = [
  { id: 'hit_rate', label: 'Hit rate' },
  { id: 'byte_hit_rate', label: 'Byte hit rate' },
]

// The charts and table for one simulation result, with the view controls that scope them.
function ResultsView({ result, policies, running, theme }) {
  const [metric, setMetric] = useState('hit_rate')
  const [seriesSize, setSeriesSize] = useState(null)

  const labels = Object.fromEntries(policies.map((p) => [p.id, p.label]))
  const offline = new Set(policies.filter((p) => p.offline).map((p) => p.id))
  const policyIds = [...new Set(result.results.map((r) => r.policy))]
  const sizes = [...new Set(result.results.map((r) => r.cache_size))]
  // The chosen size may not exist in a newer result; then show the smallest.
  const size = sizes.includes(seriesSize) ? seriesSize : sizes[0]
  const metricLabel = METRICS.find((m) => m.id === metric).label

  return (
    // While a new run is in progress this result stays, dimmed, so the layout doesn't jump.
    <section className={running ? 'results stale' : 'results'}>
      <p className="status">
        {result.results.length} runs on <strong>{result.trace}</strong>,{' '}
        {formatCount(result.results[0].requests)} requests each
        {result.warmup > 0 && ` after ${formatCount(result.warmup)} warmup requests`}, in{' '}
        {(result.elapsed_ms / 1000).toFixed(1)} s.
      </p>

      <div className="view-controls">
        <div className="segmented" role="radiogroup" aria-label="Metric">
          {METRICS.map((m) => (
            <label key={m.id} className={m.id === metric ? 'selected' : undefined}>
              <input
                type="radio"
                name="metric"
                value={m.id}
                checked={m.id === metric}
                onChange={() => setMetric(m.id)}
              />
              {m.label}
            </label>
          ))}
        </div>
        <label>
          Over-time chart at{' '}
          <select value={size} onChange={(e) => setSeriesSize(Number(e.target.value))}>
            {sizes.map((s) => (
              <option key={s} value={s}>
                {formatBytes(s)}
              </option>
            ))}
          </select>
        </label>
      </div>

      <div className="charts">
        <div className="panel">
          <h2>{metricLabel} vs cache size</h2>
          <HitRateChart
            results={result.results}
            policyIds={policyIds}
            labels={labels}
            metric={metric}
            theme={theme}
          />
        </div>
        <div className="panel">
          <h2>
            {metricLabel} over the trace at {formatBytes(size)}
          </h2>
          <p className="hint">
            Each point is the {metricLabel.toLowerCase()} of{' '}
            {formatCount(result.series_interval)} consecutive requests.
          </p>
          <SeriesChart
            results={result.results.filter((r) => r.cache_size === size)}
            interval={result.series_interval}
            policyIds={policyIds}
            labels={labels}
            metric={metric}
            theme={theme}
          />
        </div>
      </div>

      <div className="panel">
        <h2>Results</h2>
        <ResultsTable results={result.results} labels={labels} offline={offline} theme={theme} />
      </div>
    </section>
  )
}

// A small component of its own: an icon and a label, so the error is not shown by colour alone.
function ErrorBanner({ message }) {
  return (
    <div className="error" role="alert">
      <span aria-hidden="true">⚠</span> <strong>Error:</strong> {message}
    </div>
  )
}
