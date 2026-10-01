#!/usr/bin/env bash
# M3 step 9: the D18 pipeline on cloudPhysicsIO (113,872 requests). Train on the first half,
# evaluate on the second. Too small for a dev split, so nothing is tuned here: same settings
# as Twitter except every eviction is recorded (--every 1; the trace is 88x shorter).
set -euo pipefail
cd "$(dirname "$0")/.."
T=data/cloudPhysicsIO.oracleGeneral.bin
D=build/m3/cloudphysics
mkdir -p $D
SIM=build/release/apps/cachesim/cachesim
EXP=build/release/apps/export/lcache-export
TRAIN=".venv/bin/python tools/train.py"
HALF=56936
for s in 64MiB 256MiB 1GiB; do
  $SIM -t $T -p lru,wtinylfu,random-sampled,oracle-sampled,belady -c $s -w $HALF \
    -o $D/test_baselines_$s.csv
  $EXP -t $T -c $s -n $HALF --every 1 --history 10000 -o $D/lru_$s.csv 2>$D/export_lru_$s.log
  $EXP -t $T -c $s -n $HALF --every 1 --history 10000 --collector random -o $D/random_$s.csv \
    2>$D/export_random_$s.log
  $TRAIN $D/lru_$s.csv $D/random_$s.csv --window $HALF -o $D/m32x31_$s.txt >$D/train_32x31_$s.log
  $TRAIN $D/lru_$s.csv $D/random_$s.csv --window $HALF --num-trees 128 --num-leaves 63 \
    -o $D/m128x63_$s.txt >$D/train_128x63_$s.log
  for m in 32x31 128x63; do
    $SIM -t $T -p learned -c $s -w $HALF --history 10000 --model $D/m${m}_$s.txt \
      -o $D/test_learned_${m}_$s.csv
  done
done
echo "cloudphysics runs done"
