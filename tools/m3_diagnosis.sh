#!/usr/bin/env bash
# M3 diagnosis experiments (docs/m3-results.md, "Diagnosis"). Needs the v1 model from the
# steps 5-7 commands. Runs everything in parallel; about 22 minutes on 20 cores.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/m3/diag
T=data/twitter_cluster26_10M.oracleGeneral.bin
D=build/m3/diag
SIM=build/release/apps/cachesim/cachesim
EXP=build/release/apps/export/lcache-export
TRAIN=".venv/bin/python tools/train.py"
V1=build/m3/twitter_1MiB_model.txt

eval_model() {  # name model [extra cachesim args]
  local name=$1 model=$2; shift 2
  $SIM -t $T -p learned -c 1MiB -w 5000000 --model "$model" "$@" -o $D/eval_$name.csv
}

# Experiment 1a: no log_size.
( $TRAIN build/m3/twitter_train_1MiB.csv --window 5000000 --ignore-features log_size \
    -o $D/m_nosize.txt >$D/train_nosize.log && eval_model nosize $D/m_nosize.txt ) &

# Experiment 1b: unit sizes, 7,000 objects (about what 1 MiB holds at ~145 B per object).
( $EXP -t $T -c 7000 --ignore-size -n 5000000 --every 50 -o $D/train_unit.csv 2>$D/export_unit.log &&
  $TRAIN $D/train_unit.csv --window 5000000 -o $D/m_unit.txt >$D/train_unit.log &&
  $SIM -t $T -p learned -c 7000 --ignore-size -w 5000000 --model $D/m_unit.txt -o $D/eval_unit.csv ) &
$SIM -t $T -p lru,wtinylfu,random-sampled,oracle-sampled,belady -c 7000 --ignore-size -w 5000000 \
  -o $D/baselines_unit.csv &

# Experiment 2: on-policy data from v1, then retrain (on-policy only, and pooled with the LRU data).
( $EXP -t $T -c 1MiB -n 5000000 --every 50 --model $V1 -o $D/train_onpolicy.csv 2>$D/export_onpolicy.log &&
  { $TRAIN $D/train_onpolicy.csv --window 5000000 -o $D/m_onpolicy.txt >$D/train_onpolicy.log &&
      eval_model onpolicy $D/m_onpolicy.txt & } &&
  { $TRAIN build/m3/twitter_train_1MiB.csv $D/train_onpolicy.csv --window 5000000 \
      -o $D/m_pooled.txt >$D/train_pooled.log && eval_model pooled $D/m_pooled.txt & } && wait ) &

# Experiment 3: binary labels at three horizons.
for h in 6000 12000 25000; do
  ( $TRAIN build/m3/twitter_train_1MiB.csv --window 5000000 --objective binary --horizon $h \
      -o $D/m_bin$h.txt >$D/train_bin$h.log && eval_model bin$h $D/m_bin$h.txt ) &
done

wait
echo "all experiments done"
