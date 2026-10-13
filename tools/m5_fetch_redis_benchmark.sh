#!/usr/bin/env bash
# Builds redis-benchmark from Redis 7.2 into build/tools/redis-benchmark-7. The Ubuntu 22.04
# package (6.0.16) reports latency only in whole-millisecond buckets; 7.x reports p50/p99/p99.9
# from an HdrHistogram. Only the client is built; the baseline server stays the distro's
# redis-server 6.0.16. Pinned by the SHA-256 published in github.com/redis/redis-hashes.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=7.2.14
SHA256=21326da3f66c0aead4c8204c0ac52ff905337a77cadd169f75ac22835ea30025
DEST=build/tools
OUT=$DEST/redis-benchmark-7
[[ -x $OUT ]] && { echo "$OUT exists"; exit 0; }

mkdir -p "$DEST"
src=$(mktemp -d "$DEST/redis-src.XXXXXX")
trap 'rm -rf "$src"' EXIT
curl -sSLf -o "$src/redis.tar.gz" "https://download.redis.io/releases/redis-$VERSION.tar.gz"
echo "$SHA256  $src/redis.tar.gz" | sha256sum -c -
tar xzf "$src/redis.tar.gz" -C "$src"
make -C "$src/redis-$VERSION/src" -j8 MALLOC=libc BUILD_TLS=no redis-benchmark >"$src/build.log" 2>&1 ||
  { tail -30 "$src/build.log"; exit 1; }
cp "$src/redis-$VERSION/src/redis-benchmark" "$OUT"
"$OUT" --version
