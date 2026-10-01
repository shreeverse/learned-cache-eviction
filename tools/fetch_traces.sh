#!/usr/bin/env bash
# Downloads sample traces into data/ (gitignored) and verifies their checksums.
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p data

# Pinned to the libCacheSim commit we validate against (docs/m1-validation.md).
LCS_COMMIT=93529a1faef2c57bbdb9323213b78cad2adec1cd

fetch() {
  local url=$1 dest=$2 sha=$3
  if [[ ! -f $dest ]]; then
    echo "downloading $dest"
    curl -fsSL --retry 3 -o "$dest.part" "$url"
    mv "$dest.part" "$dest"
  fi
  echo "$sha  $dest" | sha256sum --check --quiet || { echo "checksum mismatch: $dest" >&2; exit 1; }
}

fetch "https://raw.githubusercontent.com/1a1a11a/libCacheSim/$LCS_COMMIT/data/cloudPhysicsIO.oracleGeneral.bin" \
  data/cloudPhysicsIO.oracleGeneral.bin \
  65438dd7b2d3dec7ffe8a5c9348045da92de3311f339b7b59c9a0a44e6050dea

echo "traces OK"
