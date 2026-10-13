import { useMemo } from 'react'
import {
  CartesianGrid,
  Legend,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts'
import { policyColor } from '../colors.js'
import { formatCompact, formatCount, formatPercent } from '../format.js'
import ChartTooltip from './ChartTooltip.jsx'

const Y_TICKS = [0, 0.25, 0.5, 0.75, 1]

// Hit rate over the trace at one cache size. The server already downsampled each run to at
// most ~200 points, each the exact hit rate of `interval` consecutive requests (decisions D26),
// so the browser never handles per-request data.
// props: results (the runs at this cache size), interval, policyIds, labels, metric, theme.
export default function SeriesChart({ results, interval, policyIds, labels, metric, theme }) {
  // One row per interval: {requests: <end of the interval>, lru: 0.48, ...}.
  const rows = useMemo(() => {
    if (results.length === 0) return []
    const total = results[0].requests
    const length = results[0].series[metric].length
    const out = []
    for (let i = 0; i < length; i += 1) {
      const row = { requests: Math.min((i + 1) * interval, total) }
      for (const r of results) row[r.policy] = r.series[metric][i]
      out.push(row)
    }
    return out
  }, [results, interval, metric])

  return (
    <ResponsiveContainer width="100%" height={340}>
      <LineChart data={rows} margin={{ top: 8, right: 24, bottom: 8, left: 0 }}>
        <CartesianGrid stroke={theme.grid} vertical={false} />
        <XAxis
          dataKey="requests"
          type="number"
          domain={[0, 'dataMax']}
          tickFormatter={formatCompact}
          stroke={theme.axis}
          tick={{ fill: theme.muted, fontSize: 12 }}
        />
        <YAxis
          domain={[0, 1]}
          ticks={Y_TICKS}
          tickFormatter={(v) => formatPercent(v, 0)}
          stroke={theme.axis}
          tick={{ fill: theme.muted, fontSize: 12 }}
          width={48}
        />
        <Tooltip
          content={<ChartTooltip formatLabel={(x) => `Requests up to ${formatCount(x)}`} />}
          cursor={{ stroke: theme.axis, strokeWidth: 1 }}
        />
        <Legend
          iconType="plainline"
          itemSorter={(item) => policyIds.indexOf(item.dataKey)} // policy order, as in the table
          formatter={(value) => <span className="legend-text">{value}</span>}
        />
        {policyIds.map((id) => (
          <Line
            key={id}
            dataKey={id}
            name={labels[id] ?? id}
            type="linear"
            stroke={policyColor(id, theme.dark)}
            strokeWidth={2}
            dot={false}
            activeDot={{ r: 4, stroke: theme.surface, strokeWidth: 2 }}
            isAnimationActive={false}
          />
        ))}
      </LineChart>
    </ResponsiveContainer>
  )
}
