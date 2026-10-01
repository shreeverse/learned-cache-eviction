#!/usr/bin/env bash
# M4 final runs on the TEST splits, after choosing grace = 30k on dev (tools/m4_guard_dev.sh).
#   guarded with the r4n models (D20), default guard settings (D21), no time budget
#   time-budget demo: Twitter 1 MiB, budget 100 us, two models (results depend on machine timing)
set -euo pipefail
cd "$(dirname "$0")/.."
F=build/m3/features
D=build/m4/test
mkdir -p $D
SIM=build/release/apps/cachesim/cachesim
TW=data/twitter_cluster26_10M.oracleGeneral.bin
for s in 512KiB 1MiB 4MiB; do
  $SIM -t $TW -p guarded -c $s -w 7500000 --model $F/m_tw_${s}_r4n.txt -o $D/guarded_tw_$s.csv \
    2>$D/guarded_tw_$s.log &
done
for s in 64MiB 256MiB 1GiB; do
  $SIM -t data/cloudPhysicsIO.oracleGeneral.bin -p guarded -c $s -w 85404 \
    --model $F/m_cp_${s}_r4n.txt -o $D/guarded_cp_$s.csv 2>$D/guarded_cp_$s.log &
done
wait
# Time budget, one run at a time so the timing isn't disturbed by the other runs.
for m in "128x63 64" "32x31 16"; do
  set -- $m
  for p in learned guarded; do
    /usr/bin/time -f "%e s" -o $D/time_${p}_$1_k$2.txt \
      $SIM -t $TW -p $p -c 1MiB -w 7500000 --sample-size $2 --budget-us 100 \
      --model models/twitter26_1MiB_h10k_$1.txt -o $D/budget_${p}_$1_k$2.csv \
      2>$D/budget_${p}_$1_k$2.log
  done
done
echo "M4 test runs done"
