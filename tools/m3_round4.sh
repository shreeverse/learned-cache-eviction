#!/usr/bin/env bash
# M3 round 4 on the DEV split: 5x training data (every 10th eviction; exports from
# tools/m3_round3.sh), with the small (32x31) and big (128x63) model. Trainings run one at a
# time (each needs several GB); the evaluations run in parallel.
set -euo pipefail
cd "$(dirname "$0")/.."
T=data/twitter_cluster26_10M.oracleGeneral.bin
D=build/m3/mw
N=10000
DEV="-w 5000000 -n 7500000"
SIM=build/release/apps/cachesim/cachesim
TRAIN=".venv/bin/python tools/train.py"
MORE="$D/lru10_h$N.csv $D/random10_h$N.csv"
dev_eval() { $SIM -t $T -p learned -c 1MiB $DEV --history $N --model $D/$1.txt -o $D/dev_$1.csv; }

$TRAIN $MORE --window 5000000 -o $D/more.txt >$D/train_more.log
dev_eval more &
$TRAIN $MORE --window 5000000 --num-trees 128 --num-leaves 63 -o $D/big_more.txt >$D/train_big_more.log
dev_eval big_more &
wait
echo "round 4 done"
