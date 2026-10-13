import { useEffect, useState } from 'react'

// Chart colours. Each policy keeps one fixed slot, whatever else is selected, so "LRU is blue"
// stays true between runs. The slot order is a validated categorical palette: neighbouring
// slots stay distinguishable under colour-vision deficiency (checked with a palette validator,
// light and dark). Light mode's aqua, yellow and magenta are below 3:1 contrast, so every value
// is also in the results table and the legend names each line.
const POLICY_ORDER = ['lru', 'lfu', 'arc', 'wtinylfu', 'belady', 'learned', 'guarded']

const SLOTS = {
  light: ['#2a78d6', '#eb6834', '#1baf7a', '#eda100', '#e87ba4', '#008300', '#6250d6', '#e34948'],
  dark: ['#3987e5', '#d95926', '#199e70', '#c98500', '#d55181', '#008300', '#9085e9', '#e66767'],
}

// Chart chrome (matches the CSS tokens in index.css; Recharts needs the values themselves).
const CHROME = {
  light: { surface: '#fcfcfb', grid: '#e1e0d9', axis: '#c3c2b7', muted: '#898781' },
  dark: { surface: '#1a1a19', grid: '#2c2c2a', axis: '#383835', muted: '#898781' },
}

export function policyColor(id, dark) {
  const slot = POLICY_ORDER.indexOf(id)
  const slots = dark ? SLOTS.dark : SLOTS.light
  return slots[slot === -1 ? slots.length - 1 : slot]
}

const DARK_QUERY = '(prefers-color-scheme: dark)'

// A custom hook: a function starting with "use" that calls other hooks, so components can share
// the logic. This one tracks the OS light/dark setting and re-renders when it changes.
function usePrefersDark() {
  const [dark, setDark] = useState(() => window.matchMedia(DARK_QUERY).matches)
  useEffect(() => {
    const query = window.matchMedia(DARK_QUERY)
    const onChange = (event) => setDark(event.matches)
    query.addEventListener('change', onChange)
    return () => query.removeEventListener('change', onChange) // unsubscribe on unmount
  }, [])
  return dark
}

// {dark, surface, grid, axis, muted} for the current colour scheme.
export function useChartTheme() {
  const dark = usePrefersDark()
  return { dark, ...(dark ? CHROME.dark : CHROME.light) }
}
