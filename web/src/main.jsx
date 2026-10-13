import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import './index.css'
import App from './App.jsx'

// Entry point: React renders the <App /> component into <div id="root"> in index.html.
// StrictMode runs extra development-only checks (e.g. it runs effects twice to expose bugs).
createRoot(document.getElementById('root')).render(
  <StrictMode>
    <App />
  </StrictMode>,
)
