import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'

// The dev server forwards /api to lcache-api, so the browser talks to a single origin and the
// API needs no CORS headers (docs/m6-design.md). LCACHE_API overrides the target.
export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      '/api': process.env.LCACHE_API ?? 'http://127.0.0.1:8080',
    },
  },
})
