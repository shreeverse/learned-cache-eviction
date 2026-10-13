// Small wrappers around fetch() for the lcache-api endpoints (docs/m6-design.md). In development,
// Vite forwards /api to lcache-api on port 8080 (vite.config.js).

const UNREACHABLE = 'Cannot reach the API. Is lcache-api running on port 8080? (see web/README.md)'

// Sends a request and returns the parsed JSON body, or throws an Error whose message is the
// server's {"error": "..."} text, so the UI can show it as is.
async function request(path, options) {
  let response
  try {
    response = await fetch(path, options)
  } catch {
    throw new Error(UNREACHABLE) // network error: nothing answered
  }
  let body = null
  try {
    body = await response.json()
  } catch {
    // not JSON, e.g. the Vite proxy's own error page when lcache-api is down
  }
  if (!response.ok) {
    if (body?.error) throw new Error(body.error)
    throw new Error(`${UNREACHABLE} (HTTP ${response.status})`)
  }
  return body
}

// [{name, format, bytes, requests}]
export function fetchTraces() {
  return request('/api/traces')
}

// [{id, label, offline, available, note}]
export function fetchPolicies() {
  return request('/api/policies')
}

// body: {trace, policies, cache_sizes, max_requests}. Resolves when every run is done.
export function runSimulation(body) {
  return request('/api/simulate', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })
}

// body: {trace, policies, slots, start, length}. Every step of a short window, for animation.
export function visualizeWindow(body) {
  return request('/api/visualize-window', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })
}
