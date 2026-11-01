#!/usr/bin/env bash
# TailTrace - one experiment: start the receiver in tt_rx, wait until it has
# joined the group, run the generator in tt_tx, collect everything into
# results/<name>/.
#
# Usage: sudo scripts/run_experiment.sh <name> [--gen "<tt_gen args>"] [--recv "<tt_recv args>"]
#   sudo scripts/run_experiment.sh poisson_100k --gen "--arrivals poisson --rate 100000 --duration 10"
#   sudo scripts/run_experiment.sh hawkes_100k  --gen "--arrivals hawkes --rate 100000 --duration 10"
#
# CPU layout (override with GEN_CPU / RECV_CPU): generator on cpu 1, receiver
# on cpu 2, so neither shares a core with the other or with cpu 0, where most
# interrupts land by default.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
name="${1:?usage: $0 <name> [--gen \"args\"] [--recv \"args\"]}"; shift
gen_args="" recv_args=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --gen) gen_args="$2"; shift 2 ;;
    --recv) recv_args="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
BIN="${BIN:-$here/build}"
GEN_CPU="${GEN_CPU:-1}" RECV_CPU="${RECV_CPU:-2}"
out="$here/results/$name"
rm -rf "$out"; mkdir -p "$out"

ip netns list | grep -q '^tt_rx' || "$here/scripts/testbed.sh" up
"$here/scripts/env.sh" "$out/env.txt"
echo "gen:  $gen_args" > "$out/cmd.txt"
echo "recv: $recv_args" >> "$out/cmd.txt"

# shellcheck disable=SC2086
ip netns exec tt_rx "$BIN/tt_recv" --cpu "$RECV_CPU" --out "$out" $recv_args > "$out/recv.log" 2>&1 &
recv_pid=$!
for _ in $(seq 100); do [[ -f "$out/ready" ]] && break; sleep 0.05; done
[[ -f "$out/ready" ]] || { echo "receiver did not start:"; cat "$out/recv.log"; exit 1; }

# shellcheck disable=SC2086
ip netns exec tt_tx "$BIN/tt_gen" --cpu "$GEN_CPU" --out "$out" $gen_args | tee "$out/gen.log"
wait "$recv_pid"
rm -f "$out/ready"
cat "$out/recv.log"
