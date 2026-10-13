#!/usr/bin/env bash
# The CI steps, shared by .github/workflows/ci.yml and local runs, so the two cannot drift apart
# (decisions D29). Run from anywhere:
#
#   tools/ci.sh cpp release|asan|tsan   # configure (warnings as errors), build, run every test
#   tools/ci.sh web                     # dashboard: npm ci, lint (warnings fail), build
#   tools/ci.sh docker                  # docker compose: build, start, query through nginx, stop
#   tools/ci.sh all                     # all of the above, as the workflow's jobs do
#
# cpp and docker need tools/fetch_traces.sh --small first (all runs it): the libCacheSim golden
# test would skip without the trace, so cpp refuses to start instead.
set -euo pipefail
cd "$(dirname "$0")/.."

# Collapsible sections in the GitHub Actions log; plain lines locally.
if [[ -n ${GITHUB_ACTIONS:-} ]]; then
  step() { echo "::group::$*"; }
  endstep() { echo "::endgroup::"; }
else
  step() { echo "==> $*"; }
  endstep() { :; }
fi

need_trace() {
  [[ -f data/cloudPhysicsIO.oracleGeneral.bin ]] || { echo "run tools/fetch_traces.sh --small" >&2; exit 1; }
}

cpp() {
  local preset=$1
  case $preset in release | asan | tsan) ;; *) echo "unknown preset '$preset'" >&2; exit 2 ;; esac
  need_trace

  # The preset's settings, in a directory of its own so a local run doesn't change build/<preset>.
  local dir=build/ci-$preset
  step "configure ($preset)"
  cmake --preset "$preset" -B "$dir" -DLC_WARNINGS_AS_ERRORS=ON
  endstep

  step "build ($preset)"
  cmake --build "$dir"
  endstep

  step "test ($preset)"
  local ctest=(ctest --test-dir "$dir" --output-on-failure)
  if [[ $preset == tsan ]]; then
    # TSan's runtime rejects the high ASLR entropy of recent kernels (WSL2, GitHub's runners);
    # running the tests with ASLR off avoids it without root (decisions D5).
    setarch "$(uname -m)" -R "${ctest[@]}"
  else
    "${ctest[@]}"
  fi
  endstep
}

web() {
  (
    cd web
    step "npm ci"
    npm ci
    endstep
    step "lint"
    npm run lint -- --deny-warnings
    endstep
    step "build"
    npm run build
    endstep
  )
}

docker_smoke() {
  need_trace
  trap 'docker compose down' EXIT

  step "docker compose up"
  docker compose up --build --detach --wait --wait-timeout 120
  endstep

  step "query through nginx (:3000)"
  curl -fsS http://127.0.0.1:3000/api/health
  echo
  curl -fsS http://127.0.0.1:3000/api/traces | grep -q cloudPhysicsIO.oracleGeneral.bin
  local out
  out=$(curl -fsS -X POST http://127.0.0.1:3000/api/simulate \
    -d '{"trace": "cloudPhysicsIO.oracleGeneral.bin", "policies": ["lru"], "cache_sizes": [67108864]}')
  echo "$out" | head -c 300
  echo
  echo "$out" | grep -q '"requests":113872' || { echo "unexpected simulate result" >&2; exit 1; }
  curl -fsS -o /dev/null http://127.0.0.1:3000/  # the dashboard itself
  endstep
}

case "${1:-}" in
  cpp) cpp "${2:-}" ;;
  web) web ;;
  docker) docker_smoke ;;
  all)
    tools/fetch_traces.sh --small
    for preset in release asan tsan; do cpp "$preset"; done
    web
    docker_smoke
    echo "all CI checks passed"
    ;;
  *)
    echo "usage: $0 cpp release|asan|tsan | web | docker | all" >&2
    exit 2
    ;;
esac
