import { useState } from 'react'
import { formatBytes, formatCount, logSpacedSizes } from '../format.js'

// React concepts used here:
// - A component is a function that returns JSX (HTML-like markup). React calls it again
//   ("re-renders") whenever its props or state change.
// - Props are the arguments a parent passes in, like <ControlPanel traces={...} />. They are
//   read-only here.
// - useState(initial) returns [value, setValue]. Calling setValue stores the new value and
//   re-renders the component. Each form field below is "controlled": its value comes from
//   state, and its onChange handler updates that state.

const MIN_EXP = 10 // 1 KiB, the API's smallest size
const MAX_EXP = 40 // 1 TiB, the API's largest size
const MAX_SIZES = 12 // the API accepts up to 12 cache sizes
const MAX_REQUESTS = 10_000_000
const DEFAULT_POLICIES = ['lru', 'arc', 'wtinylfu', 'belady']

export default function ControlPanel({ traces, policies, running, onRun }) {
  const [trace, setTrace] = useState('')
  const [selected, setSelected] = useState(DEFAULT_POLICIES)
  const [minExp, setMinExp] = useState(18) // 256 KiB
  const [maxExp, setMaxExp] = useState(26) // 64 MiB
  const [points, setPoints] = useState(5)
  const [maxRequests, setMaxRequests] = useState(1_000_000)

  // Values derived from state are computed on every render, not stored in more state.
  // Until the user picks a trace, use the first one the server listed.
  const traceName = trace || traces[0]?.name || ''
  const traceInfo = traces.find((t) => t.name === traceName)
  const sizes = logSpacedSizes(minExp, maxExp, points)
  const chosen = policies.filter((p) => p.available && selected.includes(p.id)).map((p) => p.id)
  const canRun = !running && traceName !== '' && chosen.length > 0 && maxRequests >= 1

  function togglePolicy(id) {
    setSelected((current) =>
      current.includes(id) ? current.filter((x) => x !== id) : [...current, id],
    )
  }

  // Keep min <= max: moving one slider past the other drags the other along.
  function changeMin(value) {
    setMinExp(value)
    if (value > maxExp) setMaxExp(value)
  }
  function changeMax(value) {
    setMaxExp(value)
    if (value < minExp) setMinExp(value)
  }

  function handleSubmit(event) {
    event.preventDefault() // stop the browser's default form submit (a page reload)
    if (!canRun) return
    onRun({ trace: traceName, policies: chosen, cache_sizes: sizes, max_requests: maxRequests })
  }

  return (
    <form className="panel controls" onSubmit={handleSubmit}>
      <div className="field wide">
        <label htmlFor="trace">Trace</label>
        <select id="trace" value={traceName} onChange={(e) => setTrace(e.target.value)}>
          {traces.map((t) => (
            <option key={t.name} value={t.name}>
              {t.name} ({formatCount(t.requests)} requests)
            </option>
          ))}
        </select>

        <label htmlFor="max-requests">Requests to replay</label>
        <input
          id="max-requests"
          type="number"
          min={1}
          max={MAX_REQUESTS}
          step={1} // the browser only accepts min + k * step, so a coarser step rejects 1000000
          value={maxRequests}
          onChange={(e) =>
            setMaxRequests(Math.min(MAX_REQUESTS, Math.max(0, Math.floor(Number(e.target.value)))))
          }
        />
        {traceInfo && maxRequests > traceInfo.requests && (
          <p className="hint">The trace has {formatCount(traceInfo.requests)}; all are replayed.</p>
        )}
      </div>

      <fieldset className="field">
        <legend>Policies</legend>
        {policies.map((p) => (
          <label key={p.id} className="check" title={p.note}>
            <input
              type="checkbox"
              checked={selected.includes(p.id) && p.available}
              disabled={!p.available}
              onChange={() => togglePolicy(p.id)}
            />
            {p.label}
            {p.offline && <span className="tag">offline</span>}
          </label>
        ))}
        <p className="hint">Learned runs take seconds per million requests and cache size.</p>
      </fieldset>

      <div className="field">
        <label htmlFor="min-size">Smallest cache: {formatBytes(2 ** minExp)}</label>
        <input
          id="min-size"
          type="range"
          min={MIN_EXP}
          max={MAX_EXP}
          value={minExp}
          onChange={(e) => changeMin(Number(e.target.value))}
        />
        <label htmlFor="max-size">Largest cache: {formatBytes(2 ** maxExp)}</label>
        <input
          id="max-size"
          type="range"
          min={MIN_EXP}
          max={MAX_EXP}
          value={maxExp}
          onChange={(e) => changeMax(Number(e.target.value))}
        />
        <label htmlFor="points">Sizes (log-spaced): {points}</label>
        <input
          id="points"
          type="range"
          min={1}
          max={MAX_SIZES}
          value={points}
          onChange={(e) => setPoints(Number(e.target.value))}
        />
        <div className="chips" aria-label="Cache sizes to simulate">
          {sizes.map((s) => (
            <span key={s} className="chip">
              {formatBytes(s)}
            </span>
          ))}
        </div>
      </div>

      <div className="field run">
        <button type="submit" disabled={!canRun}>
          {running ? 'Running…' : 'Run simulation'}
        </button>
        <p className="hint">
          {chosen.length * sizes.length} runs ({chosen.length} policies × {sizes.length} sizes)
        </p>
      </div>
    </form>
  )
}
