import { policyColor } from '../colors.js'
import { formatBytes, formatCount, formatPercent } from '../format.js'

// Every number behind the charts, one row per (policy, cache size). The best hit rate among
// online policies at each size is in bold (Belady, offline, is the bound they are measured
// against, so it is not a candidate).
// props: results, labels (id -> label), offline (Set of offline policy ids), theme.
export default function ResultsTable({ results, labels, offline, theme }) {
  const best = new Map() // cache size -> best online hit rate
  for (const r of results) {
    if (offline.has(r.policy)) continue
    best.set(r.cache_size, Math.max(best.get(r.cache_size) ?? 0, r.hit_rate))
  }

  return (
    <>
      {/* The table scrolls sideways on narrow screens; the note stays outside so it wraps. */}
      <div className="table-wrap">
        <table aria-describedby="results-note">
          <thead>
            <tr>
              <th scope="col">Policy</th>
              <th scope="col">Cache size</th>
              <th scope="col">Requests</th>
              <th scope="col">Hits</th>
              <th scope="col">Hit rate</th>
              <th scope="col">Byte hit rate</th>
              <th scope="col">Run time</th>
            </tr>
          </thead>
          <tbody>
            {results.map((r) => {
              const isBest = !offline.has(r.policy) && r.hit_rate === best.get(r.cache_size)
              return (
                <tr key={`${r.policy}-${r.cache_size}`}>
                  <th scope="row">
                    <span
                      className="swatch"
                      style={{ background: policyColor(r.policy, theme.dark) }}
                    />
                    {labels[r.policy] ?? r.policy}
                  </th>
                  <td>{formatBytes(r.cache_size)}</td>
                  <td>{formatCount(r.requests)}</td>
                  <td>{formatCount(r.hits)}</td>
                  <td className={isBest ? 'best' : undefined}>{formatPercent(r.hit_rate)}</td>
                  <td>{formatPercent(r.byte_hit_rate)}</td>
                  <td>{(r.elapsed_ms / 1000).toFixed(2)} s</td>
                </tr>
              )
            })}
          </tbody>
        </table>
      </div>
      <p id="results-note" className="hint table-note">
        Bold: highest hit rate among online policies at that cache size. Belady knows the future,
        so it is the upper bound rather than a competitor.
      </p>
    </>
  )
}
