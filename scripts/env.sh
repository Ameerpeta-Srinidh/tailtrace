#!/usr/bin/env bash
# TailTrace - record the machine a result came from. Two runs are only
# comparable if these match, so every results directory gets a copy.
out="${1:-/dev/stdout}"
{
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "kernel: $(uname -r)"
  echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
  echo "cpus_online: $(nproc)"
  echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
  echo "isolcpus: $(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo n/a)"
  echo "cmdline: $(cat /proc/cmdline 2>/dev/null)"
  echo "virtualised: $(systemd-detect-virt 2>/dev/null || (grep -q hypervisor /proc/cpuinfo && echo yes) || echo unknown)"
  echo "clocksource: $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null || echo n/a)"
  echo "netem: $(tc -n tt_tx qdisc show dev tt0 2>/dev/null | grep -o 'netem.*' || echo none)"
  echo "git: $(git -C "$(dirname "$0")/.." rev-parse --short HEAD 2>/dev/null || echo n/a)"
} > "$out"
