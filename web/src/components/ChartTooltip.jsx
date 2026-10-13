import { formatPercent } from '../format.js'

// Tooltip for both charts: one row per policy at the hovered x, highest value first. Recharts
// passes `active`, `payload` (one entry per line) and `label` (the x value) to this component.
export default function ChartTooltip({ active, payload, label, formatLabel }) {
  if (!active || !payload?.length) return null
  const rows = [...payload].sort((a, b) => b.value - a.value)
  return (
    <div className="tooltip">
      <div className="tooltip-title">{formatLabel(label)}</div>
      {rows.map((row) => (
        <div key={row.dataKey} className="tooltip-row">
          <span className="line-key" style={{ background: row.color }} />
          <strong>{formatPercent(row.value)}</strong>
          <span className="tooltip-name">{row.name}</span>
        </div>
      ))}
    </div>
  )
}
