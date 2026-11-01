#!/usr/bin/env bash
# TailTrace - the v0.1 experiment: Poisson vs Hawkes arrivals at the same mean
# rate, blocking vs busy-polling receiver, several seeds each (one run proves
# nothing on a shared machine; the spread between trials IS a result).
#
# Usage: sudo scripts/run_matrix.sh [rate] [duration_s] [trials]
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
rate="${1:-50000}" dur="${2:-10}" trials="${3:-3}"
for t in $(seq "$trials"); do
  for arr in poisson hawkes; do
    for mode in block busy; do
      "$here/scripts/run_experiment.sh" "${arr}_${mode}_t${t}" \
        --gen "--arrivals $arr --rate $rate --duration $dur --seed $t" \
        --recv "--mode $mode --trace-every 1 --trace-max 200000" | grep '^tt_recv'
    done
  done
done
