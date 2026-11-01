#!/usr/bin/env bash
# TailTrace - network impairment on the sender's veth with tc netem.
#
# netem is a queueing discipline ("qdisc"): every packet leaving tt0 passes
# through it, and it can delay, jitter, drop, duplicate or reorder them.
#
# Examples:
#   sudo scripts/netem.sh set delay 50us 10us        # 50 us +/- 10 us jitter
#   sudo scripts/netem.sh set loss 0.1%              # drop 1 in 1000
#   sudo scripts/netem.sh set delay 100us reorder 1% # 1% skip the delay, so jump ahead
#   sudo scripts/netem.sh set duplicate 0.5%
#   sudo scripts/netem.sh clear
set -euo pipefail
NS=tt_tx DEV=tt0
case "${1:-}" in
  set) shift; tc -n "$NS" qdisc replace dev "$DEV" root netem "$@"; tc -n "$NS" qdisc show dev "$DEV" ;;
  clear) tc -n "$NS" qdisc del dev "$DEV" root 2>/dev/null || true; echo "netem cleared" ;;
  show) tc -n "$NS" -s qdisc show dev "$DEV" ;;
  *) echo "usage: $0 set <netem args> | clear | show" >&2; exit 2 ;;
esac
