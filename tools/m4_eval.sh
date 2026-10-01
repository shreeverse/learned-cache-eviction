#!/usr/bin/env bash
# M4 / D20 evaluation on the TEST splits (models trained on the train splits by
# tools/m3_features.sh; chosen on dev):  Twitter test = 7.5M-10M;  cloudPhysicsIO test =
# 85,404-113,872 (dev was 56,936-85,404). 32x31 models, k = 64.
#   learned_old / learned_r4n   the learned policy alone (fixed 10k window raw / D20 r4n)
#   guarded_old / guarded_r4n   the same inside FallbackPolicy (D21 defaults, no time budget,
#                               so results are deterministic)
set -euo pipefail
cd "$(dirname "$0")/.."
F=build/m3/features
D=build/m4/eval
mkdir -p $D
SIM=build/release/apps/cachesim/cachesim
TW=data/twitter_cluster26_10M.oracleGeneral.bin
CP=data/cloudPhysicsIO.oracleGeneral.bin
old_model() {
  case "$1:$2" in
    tw:1MiB) echo models/twitter26_1MiB_h10k_32x31.txt ;;
    tw:*) echo build/m3/sizes/m32x31_$2.txt ;;
    cp:*) echo build/m3/cloudphysics/m32x31_$2.txt ;;
  esac
}
run() {  # tag trace warmup size
  local tag=$1 trace=$2 w=$3 s=$4
  $SIM -t $trace -p lru,wtinylfu,random-sampled,oracle-sampled,belady -c $s -w $w \
    -o $D/baselines_${tag}_$s.csv &
  for p in learned guarded; do
    $SIM -t $trace -p $p -c $s -w $w --history 10000 --model "$(old_model $tag $s)" \
      -o $D/${p}_old_${tag}_$s.csv 2>$D/${p}_old_${tag}_$s.log &
    $SIM -t $trace -p $p -c $s -w $w --model $F/m_${tag}_${s}_r4n.txt \
      -o $D/${p}_r4n_${tag}_$s.csv 2>$D/${p}_r4n_${tag}_$s.log &
  done
}
for s in 512KiB 1MiB 4MiB; do run tw $TW 7500000 $s; done
for s in 64MiB 256MiB 1GiB; do run cp $CP 85404 $s; done
wait
echo "M4 test evaluation done"
