import { useEffect, useState } from 'react'
import { fetchPolicies, fetchTraces, runSimulation } from './api.js'
import ControlPanel from './components/ControlPanel.jsx'
import { formatCount } from './format.js'

// App owns the data shared by several components (the server's lists, the last result, and
// the loading / error state) and passes it down as props ("lifting state up").
export default function App() {
  const [traces, setTraces] = useState([])
  const [policies, setPolicies] = useState([])
  const [loadError, setLoadError] = useState(null)
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

      <ControlPanel traces={traces} policies={policies} running={running} onRun={handleRun} />

      {runError && <ErrorBanner message={runError} />}
      {running && (
        <p className="status" role="status">
          Running simulation…
        </p>
      )}

      {/* While a new run is in progress the previous result stays, dimmed, so nothing jumps. */}
      {result && (
        <section className={running ? 'results stale' : 'results'}>
          <p className="status">
            {result.results.length} runs on {result.trace},{' '}
            {formatCount(result.results[0].requests)} requests each, in{' '}
            {(result.elapsed_ms / 1000).toFixed(1)} s.
          </p>
        </section>
      )}
    </div>
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
