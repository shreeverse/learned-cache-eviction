#!/usr/bin/env bash
# M3 final evaluation on the TEST split (requests 7.5M-10M; train on the first 5M; models chosen
# on the dev split 5M-7.5M before this was run). docs/m3-results.md "Final result".
#   big:    selected model (128 trees x 63 leaves, LRU + random collector data, history 10k)
#   reg_lr: pre-declared cheaper variant (32 x 31, same data), about 4x faster inference
set -euo pipefail
cd "$(dirname "$0")/.."
T=data/twitter_cluster26_10M.oracleGeneral.bin
D=build/m3/mw
SIM=build/release/apps/cachesim/cachesim
$SIM -t $T -p lru,wtinylfu,random-sampled,oracle-sampled,belady -c 1MiB -w 7500000 \
  -o $D/test_baselines.csv &
for m in big reg_lr; do
  /usr/bin/time -f "$m: %e s" $SIM -t $T -p learned -c 1MiB -w 7500000 --history 10000 \
    --model $D/$m.txt -o $D/test_$m.csv 2>>$D/test_times.log &
done
wait
echo "test evaluation done"
