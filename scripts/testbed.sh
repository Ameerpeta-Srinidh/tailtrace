#!/usr/bin/env bash
# TailTrace - laptop-scale testbed: two network namespaces joined by a veth pair.
#
#   +----------- netns tt_tx -----------+        +----------- netns tt_rx -----------+
#   |  tt_gen  -->  tt0  10.77.0.1/24   |========|   tt1  10.77.0.2/24  -->  tt_recv |
#   +-----------------------------------+  veth  +-----------------------------------+
#
# A namespace is a private copy of the network stack (interfaces, routes,
# sockets, firewall). A veth pair is a virtual cable: a packet sent into one
# end comes out of the other. Together they give two "machines" on one
# kernel, which share a clock, so one-way latency can be measured directly.
#
# Usage: sudo scripts/testbed.sh up|down|status
set -euo pipefail

TX=tt_tx RX=tt_rx
TX_IF=tt0 RX_IF=tt1
TX_IP=10.77.0.1 RX_IP=10.77.0.2

up() {
  down 2>/dev/null || true
  ip netns add "$TX"
  ip netns add "$RX"
  ip link add "$TX_IF" type veth peer name "$RX_IF"
  ip link set "$TX_IF" netns "$TX"
  ip link set "$RX_IF" netns "$RX"
  for ns in "$TX" "$RX"; do ip -n "$ns" link set lo up; done
  ip -n "$TX" addr add "$TX_IP/24" dev "$TX_IF"
  ip -n "$RX" addr add "$RX_IP/24" dev "$RX_IF"
  ip -n "$TX" link set "$TX_IF" up
  ip -n "$RX" link set "$RX_IF" up
  # Multicast (224.0.0.0/4) has no default route; point it at the veth.
  ip -n "$TX" route add 224.0.0.0/4 dev "$TX_IF"
  ip -n "$RX" route add 224.0.0.0/4 dev "$RX_IF"
  # Bigger socket buffers are allowed up to these caps (per namespace).
  ip netns exec "$RX" sysctl -qw net.core.rmem_max=33554432 2>/dev/null || true
  ip netns exec "$TX" sysctl -qw net.core.wmem_max=33554432 2>/dev/null || true
  echo "testbed up: $TX($TX_IF $TX_IP) <-> $RX($RX_IF $RX_IP)"
}

down() {
  ip netns del "$TX" 2>/dev/null || true
  ip netns del "$RX" 2>/dev/null || true   # deleting a namespace deletes its veth end, and the peer goes too
}

status() {
  for ns in "$TX" "$RX"; do
    echo "== $ns"
    ip -n "$ns" -br addr 2>/dev/null || echo "  (missing)"
    ip -n "$ns" route 2>/dev/null | sed 's/^/  route: /' || true
    tc -n "$ns" qdisc show 2>/dev/null | sed 's/^/  qdisc: /' || true
  done
}

case "${1:-}" in
  up) up ;;
  down) down; echo "testbed down" ;;
  status) status ;;
  *) echo "usage: $0 up|down|status" >&2; exit 2 ;;
esac
