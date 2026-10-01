#!/usr/bin/env bash
# M3 round 3 on the DEV split (5M-7.5M), each varying one thing vs reg_lr (tools/m3_labels.sh):
#   big:   128 trees x 63 leaves (vs 32 x 31)
#   more:  every 10th eviction recorded (vs every 50th): ~5x the rows
#   clip:  label log1p(min(distance, 100k))
#   rank:  lambdarank within each eviction's candidate set
set -euo pipefail
cd "$(dirname "$0")/.."
T=data/twitter_cluster26_10M.oracleGeneral.bin
D=build/m3/mw
N=10000
DEV="-w 5000000 -n 7500000"
SIM=build/release/apps/cachesim/cachesim
EXP=build/release/apps/export/lcache-export
TRAIN=".venv/bin/python tools/train.py"
LR="$D/lru_h$N.csv $D/random_h$N.csv"
dev_eval() { $SIM -t $T -p learned -c 1MiB $DEV --history $N --model $D/$1.txt -o $D/dev_$1.csv; }

( $TRAIN $LR --window 5000000 --num-trees 128 --num-leaves 63 -o $D/big.txt >$D/train_big.log &&
    dev_eval big ) &
( $EXP -t $T -c 1MiB -n 5000000 --every 10 --history $N -o $D/lru10_h$N.csv 2>$D/export_lru10.log &&
  $EXP -t $T -c 1MiB -n 5000000 --every 10 --history $N --collector random \
      -o $D/random10_h$N.csv 2>$D/export_random10.log &&
  $TRAIN $D/lru10_h$N.csv $D/random10_h$N.csv --window 5000000 -o $D/more.txt >$D/train_more.log &&
  dev_eval more ) &
( $TRAIN $LR --window 5000000 --clip-distance 100000 -o $D/clip.txt >$D/train_clip.log &&
    dev_eval clip ) &
( $TRAIN $LR --window 5000000 --objective lambdarank -o $D/rank.txt >$D/train_rank.log &&
    dev_eval rank ) &
wait
echo "round 3 done"
