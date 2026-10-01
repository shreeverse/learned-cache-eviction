#!/usr/bin/env bash
# D20 feature experiment, DEV splits only (selection; the test splits are run once afterwards).
#   Twitter: train 0-5M, dev 5M-7.5M.  cloudPhysicsIO: train 0-56,936, dev 56,936-85,404.
# Variants (32x31 model, k = 64, LRU + random collector data as in D18):
#   old    fixed 10k window, raw features (the existing models)
#   r2raw  window 2 x cached objects, raw
#   r1n / r2n / r4n  window 1 / 2 / 4 x cached objects, normalized
set -euo pipefail
cd "$(dirname "$0")/.."
D=build/m3/features
mkdir -p $D
SIM=build/release/apps/cachesim/cachesim
EXP=build/release/apps/export/lcache-export
TRAIN=".venv/bin/python tools/train.py"
TW=data/twitter_cluster26_10M.oracleGeneral.bin
CP=data/cloudPhysicsIO.oracleGeneral.bin
declare -A FLAGS=([r2raw]="--history-ratio 2" [r1n]="--history-ratio 1 --normalize"
                  [r2n]="--history-ratio 2 --normalize" [r4n]="--history-ratio 4 --normalize")
# trace-tag  trace  train-requests  dev-window  every  sizes
CONFIGS=("tw $TW 5000000 -w_5000000_-n_7500000 50 512KiB 1MiB 4MiB"
         "cp $CP 56936 -w_56936_-n_85404 1 64MiB 256MiB 1GiB")

# 1. Exports (parallel), and dev baselines.
for cfg in "${CONFIGS[@]}"; do
  read -r tag trace ntrain dev every sizes <<<"$cfg"; sizes=$(echo "$cfg" | cut -d' ' -f6-)
  for s in $sizes; do
    $SIM -t $trace -p lru,wtinylfu,random-sampled,oracle-sampled,belady -c $s ${dev//_/ } \
      -o $D/dev_baselines_${tag}_$s.csv &
    for v in "${!FLAGS[@]}"; do
      for c in lru random; do
        $EXP -t $trace -c $s -n $ntrain --every $every --collector $c ${FLAGS[$v]} \
          -o $D/${c}_${tag}_${s}_$v.csv 2>$D/export_${c}_${tag}_${s}_$v.log &
      done
    done
  done
done
wait

# 2. Training, three at a time (each holds a few GB).
for cfg in "${CONFIGS[@]}"; do
  read -r tag trace ntrain dev every sizes <<<"$cfg"; sizes=$(echo "$cfg" | cut -d' ' -f6-)
  for s in $sizes; do for v in "${!FLAGS[@]}"; do echo "$tag $s $v $ntrain"; done; done
done | xargs -P 3 -L 1 bash -c '$0 '"$D"'/lru_$1_$2_$3.csv '"$D"'/random_$1_$2_$3.csv --window $4 \
    -o '"$D"'/m_$1_$2_$3.txt >'"$D"'/train_$1_$2_$3.log' "$TRAIN"

# 3. Dev evaluations (parallel). Models carry their settings in .meta.
old_model() {  # tag size -> the existing fixed-10k model
  case "$1:$2" in
    tw:1MiB) echo models/twitter26_1MiB_h10k_32x31.txt ;;
    tw:*) echo build/m3/sizes/m32x31_$2.txt ;;
    cp:*) echo build/m3/cloudphysics/m32x31_$2.txt ;;
  esac
}
for cfg in "${CONFIGS[@]}"; do
  read -r tag trace ntrain dev every sizes <<<"$cfg"; sizes=$(echo "$cfg" | cut -d' ' -f6-)
  for s in $sizes; do
    $SIM -t $trace -p learned -c $s ${dev//_/ } --history 10000 --model "$(old_model $tag $s)" \
      -o $D/dev_${tag}_${s}_old.csv &
    for v in "${!FLAGS[@]}"; do
      $SIM -t $trace -p learned -c $s ${dev//_/ } --model $D/m_${tag}_${s}_$v.txt \
        -o $D/dev_${tag}_${s}_$v.csv &
    done
  done
done
wait
echo "feature experiments done"
