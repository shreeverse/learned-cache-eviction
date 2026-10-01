#!/usr/bin/env bash
# M3 follow-up to tools/m3_memory_window.sh (history 10k), docs/m3-results.md "Memory window".
#   mw3:     a second on-policy round (mw2 drives the cache), pooled with all earlier data
#   rnd:     LRU-collector data + random-collector data (covers old keys)
#   rnd_op:  rnd + the first on-policy round
set -euo pipefail
cd "$(dirname "$0")/.."
T=data/twitter_cluster26_10M.oracleGeneral.bin
D=build/m3/mw
N=10000
SIM=build/release/apps/cachesim/cachesim
EXP=build/release/apps/export/lcache-export
TRAIN=".venv/bin/python tools/train.py"
evaluate() { $SIM -t $T -p learned -c 1MiB -w 5000000 --history $N --model $D/$1.txt -o $D/eval_$1.csv; }

( $EXP -t $T -c 1MiB -n 5000000 --every 50 --history $N --model $D/mw2_h$N.txt \
      -o $D/onpolicy2_h$N.csv 2>$D/export_onpolicy2_h$N.log &&
  $TRAIN $D/lru_h$N.csv $D/onpolicy_h$N.csv $D/onpolicy2_h$N.csv --window 5000000 \
      -o $D/mw3.txt >$D/train_mw3.log && evaluate mw3 ) &

( $EXP -t $T -c 1MiB -n 5000000 --every 50 --history $N --collector random \
      -o $D/random_h$N.csv 2>$D/export_random_h$N.log &&
  { $TRAIN $D/lru_h$N.csv $D/random_h$N.csv --window 5000000 -o $D/rnd.txt >$D/train_rnd.log &&
      evaluate rnd & } &&
  { $TRAIN $D/lru_h$N.csv $D/random_h$N.csv $D/onpolicy_h$N.csv --window 5000000 \
      -o $D/rnd_op.txt >$D/train_rnd_op.log && evaluate rnd_op & } && wait ) &

wait
echo "round 2 done"
