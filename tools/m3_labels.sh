#!/usr/bin/env bash
# M3 label/data experiments on the DEV split (requests 5M-7.5M; train on the first 5M),
# docs/m3-results.md "Memory window". Uses exports from tools/m3_memory_window.sh and
# tools/m3_round2.sh (history 10k).
#   reg_lr:    regression,  LRU + random collector data   (= "rnd" there)
#   bin12k_lr: binary label, distance > 12,000, same data
#   bin25k_lr: binary label, distance > 25,000, same data
#   reg_r:     regression,  random collector data only
set -euo pipefail
cd "$(dirname "$0")/.."
T=data/twitter_cluster26_10M.oracleGeneral.bin
D=build/m3/mw
N=10000
DEV="-w 5000000 -n 7500000"
SIM=build/release/apps/cachesim/cachesim
TRAIN=".venv/bin/python tools/train.py"
LR="$D/lru_h$N.csv $D/random_h$N.csv"
dev_eval() { $SIM -t $T -p learned -c 1MiB $DEV --history $N --model $D/$1.txt -o $D/dev_$1.csv; }

$SIM -t $T -p lru,wtinylfu,random-sampled,oracle-sampled,belady -c 1MiB $DEV -o $D/dev_baselines.csv &
( $TRAIN $LR --window 5000000 -o $D/reg_lr.txt >$D/train_reg_lr.log && dev_eval reg_lr ) &
( $TRAIN $LR --window 5000000 --objective binary --horizon 12000 -o $D/bin12k_lr.txt \
    >$D/train_bin12k_lr.log && dev_eval bin12k_lr ) &
( $TRAIN $LR --window 5000000 --objective binary --horizon 25000 -o $D/bin25k_lr.txt \
    >$D/train_bin25k_lr.log && dev_eval bin25k_lr ) &
( $TRAIN $D/random_h$N.csv --window 5000000 -o $D/reg_r.txt >$D/train_reg_r.log && dev_eval reg_r ) &
wait
echo "label experiments done"
