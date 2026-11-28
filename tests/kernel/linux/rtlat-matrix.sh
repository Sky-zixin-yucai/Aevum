#!/usr/bin/env bash
# 跑 Linux i386 SCHED_FIFO 周期唤醒延迟矩阵（3 次/后台数）。
# 依赖已构建的 bzImage 与 rtlat initramfs。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RES="${RES:-/mnt/d/Aevum/tests/kernel/results/2026-11-28}"
BZIMAGE="${BZIMAGE:-/root/aevum-test/out/linux-i386-hz100/bzImage}"
ROOTFS="${AEVUM_TEST_ROOTFS:-/root/aevum-test/rootfs-i386-rtlat.cpio.gz}"

for bg in 2 4 8; do
  for r in 1 2 3; do
    log="$RES/linux-rtlat-bg${bg}-r${r}.log"
    BZIMAGE="$BZIMAGE" AEVUM_TEST_ROOTFS="$ROOTFS" \
      AEVUM_TEST_BENCH="bench_policy=rtlat bench_tasks=$bg bench_rt_period_us=10000 bench_rt_work_us=1500 bench_rt_periods=1000 bench_rt_deadline_us=10000" \
      OUT="$log" AEVUM_TEST_TIMEOUT=45 "$HERE/run-qemu.sh" \
      > "/tmp/linux-rtlat-bg${bg}-r${r}.console" 2>&1
    printf 'bg=%s r=%s ' "$bg" "$r"
    grep 'RT_STAT' "$log" | tail -1
  done
done
