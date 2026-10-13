import { useEffect, useMemo, useState } from 'react'
import { visualizeWindow } from '../api.js'
import { policyColor } from '../colors.js'
import { formatCount } from '../format.js'

// Animates a short window of a trace: which keys each policy keeps in its slots, and which it
// evicts, request by request. The server computes every step (POST /api/visualize-window,
// decisions D27); this component only draws step `step` of that answer.

const MAX_POLICIES = 4
const SPEEDS = [0.5, 1, 2, 4] // steps per second

export default function EvictionVisualizer({ traces, policies, theme }) {
  const [trace, setTrace] = useState('')
  const [selected, setSelected] = useState(['lru', 'belady'])
  const [slots, setSlots] = useState(8)
  const [start, setStart] = useState(0)
  const [length, setLength] = useState(60)
  const [data, setData] = useState(null)
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState(null)
  // Playback: `step` is how many requests of the window have been applied (0 = the cache as it
  // was when the window starts, data.length = after the last request).
  const [step, setStep] = useState(0)
  const [playing, setPlaying] = useState(false)
  const [speed, setSpeed] = useState(2)

  const traceName = trace || traces[0]?.name || ''
  const available = policies.filter((p) => p.available)
  const chosen = available.filter((p) => selected.includes(p.id)).map((p) => p.id)
  const labels = Object.fromEntries(policies.map((p) => [p.id, p.label]))
  const total = data ? data.requests.length : 0
  // Derived, not stored: playback stops by itself at the end of the window.
  const isPlaying = playing && step < total

  // The timer for playback. While playing, an interval advances the step; the cleanup function
  // clears it when playback stops (including at the end), the speed changes or the component
  // goes away, so there is never more than one timer.
  useEffect(() => {
    if (!isPlaying) return undefined
    const id = setInterval(() => {
      setStep((s) => Math.min(s + 1, total))
    }, 1000 / speed)
    return () => clearInterval(id)
  }, [isPlaying, speed, total])

  // Hits so far for each policy at each step: hitsBefore[i][k] = hits in the first k requests.
  const hitsBefore = useMemo(() => {
    if (!data) return []
    return data.policies.map((p) => {
      const out = [0]
      for (const s of p.steps) out.push(out.at(-1) + (s.hit ? 1 : 0))
      return out
    })
  }, [data])

  function togglePolicy(id) {
    setSelected((current) => {
      if (current.includes(id)) return current.filter((x) => x !== id)
      return current.length >= MAX_POLICIES ? current : [...current, id]
    })
  }

  async function handleLoad(event) {
    event.preventDefault()
    setLoading(true)
    setError(null)
    setPlaying(false)
    try {
      const result = await visualizeWindow({
        trace: traceName,
        policies: chosen,
        slots,
        start,
        length,
      })
      setData(result)
      setStep(0)
    } catch (e) {
      setError(e.message)
    } finally {
      setLoading(false)
    }
  }

  const current = step > 0 && data ? data.requests[step - 1] : null // the key just requested

  return (
    <div className="visualizer">
      <form className="panel controls" onSubmit={handleLoad}>
        <div className="field wide">
          <label htmlFor="viz-trace">Trace</label>
          <select id="viz-trace" value={traceName} onChange={(e) => setTrace(e.target.value)}>
            {traces.map((t) => (
              <option key={t.name} value={t.name}>
                {t.name} ({formatCount(t.requests)} requests)
              </option>
            ))}
          </select>
          <label htmlFor="viz-start">Window starts after request</label>
          <input
            id="viz-start"
            type="number"
            min={0}
            max={1_000_000}
            step={1}
            value={start}
            onChange={(e) =>
              setStart(Math.min(1_000_000, Math.max(0, Math.floor(Number(e.target.value)))))
            }
          />
          <p className="hint">Earlier requests warm the cache but are not shown.</p>
        </div>

        <fieldset className="field">
          <legend>Policies (up to {MAX_POLICIES})</legend>
          {available.map((p) => (
            <label key={p.id} className="check">
              <input
                type="checkbox"
                checked={selected.includes(p.id)}
                disabled={!selected.includes(p.id) && chosen.length >= MAX_POLICIES}
                onChange={() => togglePolicy(p.id)}
              />
              {p.label}
              {p.offline && <span className="tag">offline</span>}
            </label>
          ))}
        </fieldset>

        <div className="field">
          <label htmlFor="viz-slots">Cache slots: {slots} objects</label>
          <input
            id="viz-slots"
            type="range"
            min={2}
            max={16}
            value={slots}
            onChange={(e) => setSlots(Number(e.target.value))}
          />
          <label htmlFor="viz-length">Window: {length} requests</label>
          <input
            id="viz-length"
            type="range"
            min={10}
            max={100}
            step={10}
            value={length}
            onChange={(e) => setLength(Number(e.target.value))}
          />
          <p className="hint">
            The capacity counts objects, so every request counts as size 1. The learned model was
            trained on byte sizes at 1 MiB, so it is out of its depth here.
          </p>
        </div>

        <div className="field run">
          <button type="submit" disabled={loading || chosen.length === 0 || traceName === ''}>
            {loading ? 'Loading…' : 'Load window'}
          </button>
        </div>
      </form>

      {error && (
        <div className="error" role="alert">
          <span aria-hidden="true">⚠</span> <strong>Error:</strong> {error}
        </div>
      )}

      {data && (
        <section className={loading ? 'panel window stale' : 'panel window'}>
          <Player
            step={step}
            total={total}
            playing={isPlaying}
            speed={speed}
            onStep={(s) => {
              setPlaying(false)
              setStep(Math.max(0, Math.min(total, s)))
            }}
            onPlay={() => {
              if (step >= total) {
                setStep(0) // at the end: replay from the start
                setPlaying(true)
              } else {
                setPlaying(!isPlaying)
              }
            }}
            onSpeed={setSpeed}
          />

          <RequestStrip requests={data.requests} step={step} />

          {data.policies.map((p, i) => (
            <Lane
              key={p.policy}
              label={labels[p.policy] ?? p.policy}
              color={policyColor(p.policy, theme.dark)}
              run={p}
              step={step}
              current={current}
              hits={hitsBefore[i][step]}
            />
          ))}
          <p className="hint">
            {data.slots} slots, requests {formatCount(data.start + 1)}–
            {formatCount(data.start + data.requests.length)} of {data.trace}. Letters stand for keys
            in order of first appearance.
          </p>
        </section>
      )}
    </div>
  )
}

// Playback controls: reset, step back, play / pause, step forward, speed, and a scrubber.
function Player({ step, total, playing, speed, onStep, onPlay, onSpeed }) {
  return (
    <div className="player">
      <div className="player-buttons">
        <button
          type="button"
          className="ghost"
          onClick={() => onStep(0)}
          aria-label="Back to start"
        >
          Start
        </button>
        <button
          type="button"
          className="ghost"
          onClick={() => onStep(step - 1)}
          aria-label="Step back"
        >
          ◀
        </button>
        <button type="button" onClick={onPlay} aria-label={playing ? 'Pause' : 'Play'}>
          {playing ? '⏸ Pause' : '▶ Play'}
        </button>
        <button
          type="button"
          className="ghost"
          onClick={() => onStep(step + 1)}
          aria-label="Step forward"
        >
          ▶
        </button>
      </div>
      <label className="player-speed">
        Speed
        <select value={speed} onChange={(e) => onSpeed(Number(e.target.value))}>
          {SPEEDS.map((s) => (
            <option key={s} value={s}>
              {s} / s
            </option>
          ))}
        </select>
      </label>
      <input
        className="scrubber"
        type="range"
        min={0}
        max={total}
        value={step}
        onChange={(e) => onStep(Number(e.target.value))}
        aria-label="Step"
      />
      <span className="player-count">
        {step} / {total}
      </span>
    </div>
  )
}

// The window's requests as a strip of keys; the one just applied is marked.
function RequestStrip({ requests, step }) {
  return (
    <ol className="strip" aria-label="Requests in the window">
      {requests.map((key, i) => {
        const state = i === step - 1 ? 'current' : i < step - 1 ? 'past' : 'future'
        return (
          <li
            key={i}
            className={`strip-key ${state}`}
            aria-current={state === 'current' ? 'step' : undefined}
          >
            {key}
          </li>
        )
      })}
    </ol>
  )
}

// One policy: its slots after `step` requests, the outcome of the last request, and what it has
// evicted so far.
function Lane({ label, color, run, step, current, hits }) {
  const slots = step === 0 ? run.initial : run.steps[step - 1].slots
  const last = step === 0 ? null : run.steps[step - 1]
  const evicted = run.steps.slice(0, step).flatMap((s) => s.evicted)

  return (
    <div className="lane">
      <div className="lane-head">
        <span className="swatch" style={{ background: color }} />
        <strong>{label}</strong>
        <span className="lane-hits">
          {hits} hit{hits === 1 ? '' : 's'} / {step}
        </span>
        {last && (
          <span className={last.hit ? 'outcome hit' : 'outcome miss'}>
            {last.hit ? '✓ hit' : '✗ miss'}
            {last.evicted.length > 0 && ` · evicted ${last.evicted.join(', ')}`}
          </span>
        )}
      </div>
      <div className="slots">
        {slots.map((key, i) => (
          <div key={i} className={key !== null && key === current ? 'slot accessed' : 'slot'}>
            {/* key={...} makes React create a new element when the slot's content changes, so
                the CSS entry animation plays for each newly inserted key. */}
            {key !== null && (
              <span key={key} className="slot-key">
                {key}
              </span>
            )}
          </div>
        ))}
      </div>
      <div className="evicted" aria-label="Evicted so far, most recent first">
        Evicted:{' '}
        {evicted.length === 0
          ? 'none yet'
          : evicted
              .slice(-12)
              .reverse()
              .map((k, i) => (
                <s key={i} className="evicted-key">
                  {k}
                </s>
              ))}
        {evicted.length > 12 && <span className="hint"> +{evicted.length - 12} earlier</span>}
      </div>
    </div>
  )
}
