// Number formatting and cache-size helpers shared by the components.

const UNITS = ['B', 'KiB', 'MiB', 'GiB', 'TiB']

// 1048576 -> "1 MiB", 2642412 -> "2.52 MiB" (binary units, like cachesim's --cache-size).
export function formatBytes(bytes) {
  let value = bytes
  let unit = 0
  while (value >= 1024 && unit < UNITS.length - 1) {
    value /= 1024
    unit += 1
  }
  const text = Number.isInteger(value) ? String(value) : String(Number(value.toPrecision(3)))
  return `${text} ${UNITS[unit]}`
}

const countFormat = new Intl.NumberFormat('en-US')
const compactFormat = new Intl.NumberFormat('en-US', {
  notation: 'compact',
  maximumFractionDigits: 1,
})

// 1234567 -> "1,234,567"
export function formatCount(n) {
  return countFormat.format(n)
}

// 1234567 -> "1.2M" (axis ticks)
export function formatCompact(n) {
  return compactFormat.format(n)
}

// 0.50303 -> "50.30%"
export function formatPercent(fraction, digits = 2) {
  return `${(fraction * 100).toFixed(digits)}%`
}

// `points` cache sizes evenly spaced on a log scale from 2^minExp to 2^maxExp bytes, rounded
// to whole KiB, ascending and without duplicates. Hit rate vs cache size is usually plotted on
// a log axis, so even spacing there gives one point per "zoom level".
export function logSpacedSizes(minExp, maxExp, points) {
  if (points <= 1 || minExp === maxExp) return [2 ** minExp]
  const sizes = []
  for (let i = 0; i < points; i += 1) {
    const exp = minExp + (i * (maxExp - minExp)) / (points - 1)
    const bytes = Math.max(1024, Math.round(2 ** exp / 1024) * 1024)
    if (sizes.at(-1) !== bytes) sizes.push(bytes)
  }
  return sizes
}
