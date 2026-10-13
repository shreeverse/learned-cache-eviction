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
import { formatBytes, formatPercent } from '../format.js'
import ChartTooltip from './ChartTooltip.jsx'

const Y_TICKS = [0, 0.25, 0.5, 0.75, 1]

// Hit rate (or byte hit rate) vs cache size: one line per policy, log-scale x axis.
// props: results (the API's results array), policyIds (in display order), labels (id -> label),
// metric ('hit_rate' | 'byte_hit_rate'), theme (from useChartTheme).
export default function HitRateChart({ results, policyIds, labels, metric, theme }) {
  // Recharts wants one object per x value: {cache_size, lru: 0.5, arc: 0.51, ...}.
  // useMemo caches this until `results` or `metric` change, instead of rebuilding it on every
  // render (e.g. while the pointer moves over the chart).
  const { rows, sizes } = useMemo(() => {
    const byKey = new Map()
    for (const r of results) {
      if (!byKey.has(r.cache_size)) byKey.set(r.cache_size, { cache_size: r.cache_size })
      byKey.get(r.cache_size)[r.policy] = r[metric]
    }
    const sorted = [...byKey.values()].sort((a, b) => a.cache_size - b.cache_size)
    return { rows: sorted, sizes: sorted.map((row) => row.cache_size) }
  }, [results, metric])

  // A log scale needs two distinct sizes; a single size is plotted as one category.
  const xAxis =
    sizes.length > 1
      ? { type: 'number', scale: 'log', domain: ['dataMin', 'dataMax'], ticks: sizes }
      : { type: 'category' }

  return (
    <ResponsiveContainer width="100%" height={340}>
      <LineChart data={rows} margin={{ top: 8, right: 24, bottom: 8, left: 0 }}>
        <CartesianGrid stroke={theme.grid} vertical={false} />
        <XAxis
          dataKey="cache_size"
          {...xAxis}
          tickFormatter={formatBytes}
          stroke={theme.axis}
          tick={{ fill: theme.muted, fontSize: 12 }}
          padding={{ left: 12, right: 12 }}
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
          content={<ChartTooltip formatLabel={formatBytes} />}
          cursor={{ stroke: theme.axis, strokeWidth: 1 }}
        />
        <Legend
          iconType="plainline"
          itemSorter={(item) => policyIds.indexOf(item.dataKey)} // policy order, as in the table
          formatter={(value) => <span className="legend-text">{value}</span>}
        />
        {policyIds.map((id) => {
          const color = policyColor(id, theme.dark)
          return (
            <Line
              key={id}
              dataKey={id}
              name={labels[id] ?? id}
              type="linear"
              stroke={color}
              strokeWidth={2}
              dot={{ r: 4, fill: color, stroke: theme.surface, strokeWidth: 2 }}
              activeDot={{ r: 6, fill: color, stroke: theme.surface, strokeWidth: 2 }}
              isAnimationActive={false}
            />
          )
        })}
      </LineChart>
    </ResponsiveContainer>
  )
}
