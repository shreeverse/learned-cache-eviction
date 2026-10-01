#!/usr/bin/env bash
# M4: choose the guard's grace period / early check on the DEV splits (r4n models, D20).
#   g0   grace 0, early check (the first version)
#   g1   grace 10k, early check
#   g3   grace 30k, early check
#   g1n  grace 10k, no early check
set -euo pipefail
cd "$(dirname "$0")/.."
F=build/m3/features
D=build/m4/guard_dev
mkdir -p $D
SIM=build/release/apps/cachesim/cachesim
declare -A V=([g0]="--guard-grace 0" [g1]="--guard-grace 10000" [g3]="--guard-grace 30000"
              [g1n]="--guard-grace 10000 --no-early-check")
for cfg in "tw data/twitter_cluster26_10M.oracleGeneral.bin -w_5000000_-n_7500000 512KiB 1MiB 4MiB" \
           "cp data/cloudPhysicsIO.oracleGeneral.bin -w_56936_-n_85404 64MiB 256MiB 1GiB"; do
  read -r tag trace dev sizes <<<"$cfg"
  for s in $sizes; do
    for v in "${!V[@]}"; do
      $SIM -t $trace -p guarded -c $s ${dev//_/ } --model $F/m_${tag}_${s}_r4n.txt ${V[$v]} \
        -o $D/${v}_${tag}_$s.csv 2>$D/${v}_${tag}_$s.log &
    done
  done
done
wait
echo "guard dev sweep done"
