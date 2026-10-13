#!/usr/bin/env bash
# Downloads sample traces into data/ (gitignored) and verifies their checksums.
#   tools/fetch_traces.sh           # cloudPhysicsIO (2.7 MB) and Twitter cluster26 (784 MB download)
#   tools/fetch_traces.sh --small   # cloudPhysicsIO only (what CI needs for the golden test)
set -euo pipefail

small=false
case "${1:-}" in
  "") ;;
  --small) small=true ;;
  *) echo "usage: $0 [--small]" >&2; exit 2 ;;
esac

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

if $small; then
  echo "traces OK (cloudPhysicsIO only)"
  exit 0
fi

# Twitter cluster26 (Yang et al., OSDI '20), from the CMU PDL mirror that libCacheSim links to.
# The full trace is 784 MB compressed; we keep the first 10M requests (24-byte records) as an
# uncompressed file. A contiguous prefix keeps time order, which the train-on-first-half
# evaluation needs (docs/decisions.md D10, D15).
TW_REQUESTS=10000000
TW_BIN=data/twitter_cluster26_10M.oracleGeneral.bin
TW_BIN_SHA=75136f41ebb6b5ff1b090d90b075f3879e7b15a9e811359ace0e2230d09186a1
if [[ ! -f $TW_BIN ]]; then
  command -v zstd >/dev/null || { echo "zstd is required: sudo apt install zstd" >&2; exit 1; }
  fetch "https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/twitter/cluster26.oracleGeneral.zst" \
    data/cluster26.oracleGeneral.zst \
    a52dd19053fb8eb36551c76f5ab072b4fede381c6c5241f185a98c761d7d07d0
  echo "extracting the first $TW_REQUESTS requests into $TW_BIN"
  # head stops reading early, so zstd exits on SIGPIPE; that's expected (the checksum below
  # catches a short or corrupt file).
  { zstd -dc data/cluster26.oracleGeneral.zst || true; } | head -c $((24 * TW_REQUESTS)) >"$TW_BIN.part"
  mv "$TW_BIN.part" "$TW_BIN"
fi
echo "$TW_BIN_SHA  $TW_BIN" | sha256sum --check --quiet || { echo "checksum mismatch: $TW_BIN" >&2; exit 1; }

echo "traces OK"
