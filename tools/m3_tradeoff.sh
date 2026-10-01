#!/usr/bin/env bash
# M3 step 8: hit rate for each (model, sample size k) on the DEV split (5M-7.5M) of Twitter at
# 1 MiB, to pair with the per-eviction latency from bench/eviction_bench. Compiled predictor.
set -euo pipefail
cd "$(dirname "$0")/.."
T=data/twitter_cluster26_10M.oracleGeneral.bin
D=build/m3/tradeoff
mkdir -p $D
SIM=build/release/apps/cachesim/cachesim
for m in 32x31 128x63; do
  for k in 16 32 64; do
    $SIM -t $T -p learned -c 1MiB -w 5000000 -n 7500000 --history 10000 --sample-size $k \
      --model models/twitter26_1MiB_h10k_$m.txt -o $D/dev_${m}_k$k.csv &
  done
done
$SIM -t $T -p random-sampled -c 1MiB -w 5000000 -n 7500000 --sample-size 16 -o $D/dev_random_k16.csv &
$SIM -t $T -p random-sampled -c 1MiB -w 5000000 -n 7500000 --sample-size 32 -o $D/dev_random_k32.csv &
wait
echo "trade-off runs done"
