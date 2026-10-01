#!/usr/bin/env bash
# M3 memory-window experiments (docs/m3-results.md, "Memory window"). For each window size N:
#   mw1 = trained on LRU-collector data exported with --history N
#   mw2 = trained on that data pooled with on-policy data (mw1 driving the cache)
# Every model is evaluated with the same --history N it was trained with (decisions D17).
# Chains for different N run in parallel.
set -euo pipefail
cd "$(dirname "$0")/.."
T=data/twitter_cluster26_10M.oracleGeneral.bin
D=build/m3/mw
mkdir -p $D
SIM=build/release/apps/cachesim/cachesim
EXP=build/release/apps/export/lcache-export
TRAIN=".venv/bin/python tools/train.py"
WINDOWS=${WINDOWS:-"10000 30000 100000"}

chain() {
  local n=$1
  $EXP -t $T -c 1MiB -n 5000000 --every 50 --history $n -o $D/lru_h$n.csv 2>$D/export_lru_h$n.log
  $TRAIN $D/lru_h$n.csv --window 5000000 -o $D/mw1_h$n.txt >$D/train_mw1_h$n.log
  $SIM -t $T -p learned -c 1MiB -w 5000000 --history $n --model $D/mw1_h$n.txt \
    -o $D/eval_mw1_h$n.csv &
  $EXP -t $T -c 1MiB -n 5000000 --every 50 --history $n --model $D/mw1_h$n.txt \
    -o $D/onpolicy_h$n.csv 2>$D/export_onpolicy_h$n.log
  $TRAIN $D/lru_h$n.csv $D/onpolicy_h$n.csv --window 5000000 -o $D/mw2_h$n.txt \
    >$D/train_mw2_h$n.log
  $SIM -t $T -p learned -c 1MiB -w 5000000 --history $n --model $D/mw2_h$n.txt \
    -o $D/eval_mw2_h$n.csv
  wait
}

for n in $WINDOWS; do chain "$n" & done
wait
echo "memory-window experiments done"
