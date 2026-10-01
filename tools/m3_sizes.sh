#!/usr/bin/env bash
# M3 step 9: other cache sizes on Twitter, TEST split (7.5M-10M), with the configuration fixed
# in D18 (LRU + random collector data, history 10k). Per size:
#   transfer: the 1 MiB models (models/) used as-is
#   retrained: same pipeline, exported and trained at that size
set -euo pipefail
cd "$(dirname "$0")/.."
T=data/twitter_cluster26_10M.oracleGeneral.bin
D=build/m3/sizes
mkdir -p $D
SIM=build/release/apps/cachesim/cachesim
EXP=build/release/apps/export/lcache-export
TRAIN=".venv/bin/python tools/train.py"
SIZES=${SIZES:-"512KiB 2MiB 4MiB"}
TEST="-w 7500000"
learned() {  # size model out
  $SIM -t $T -p learned -c $1 $TEST --history 10000 --model $2 -o $3
}

for s in $SIZES; do
  $SIM -t $T -p lru,wtinylfu,random-sampled,oracle-sampled,belady -c $s $TEST -o $D/test_baselines_$s.csv &
  for m in 32x31 128x63; do learned $s models/twitter26_1MiB_h10k_$m.txt $D/test_transfer_${m}_$s.csv & done
  $EXP -t $T -c $s -n 5000000 --every 50 --history 10000 -o $D/lru_$s.csv 2>$D/export_lru_$s.log &
  $EXP -t $T -c $s -n 5000000 --every 50 --history 10000 --collector random -o $D/random_$s.csv \
    2>$D/export_random_$s.log &
done
wait  # exports (and the fast runs) done; train one at a time (several GB each)
for s in $SIZES; do
  $TRAIN $D/lru_$s.csv $D/random_$s.csv --window 5000000 -o $D/m32x31_$s.txt >$D/train_32x31_$s.log
  learned $s $D/m32x31_$s.txt $D/test_retrained_32x31_$s.csv &
  $TRAIN $D/lru_$s.csv $D/random_$s.csv --window 5000000 --num-trees 128 --num-leaves 63 \
    -o $D/m128x63_$s.txt >$D/train_128x63_$s.log
  learned $s $D/m128x63_$s.txt $D/test_retrained_128x63_$s.csv &
done
wait
echo "size runs done"
