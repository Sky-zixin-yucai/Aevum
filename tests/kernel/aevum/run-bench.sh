#!/usr/bin/env bash
# A0 真实 QEMU 基准：4 个 ring3 CPU-bound 任务，优先级 0..3。
# 只在 WSL/Linux 里跑；产物写到 tests/kernel/results 下。
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
ROBOT="$ROOT/src/kernel/host/console/robot"
OUT="${1:-$ROOT/tests/kernel/results/2026-11-26/aevum-a0}"
REPS="${REPS:-3}"
TASKS="${BENCH_TASKS:-4}"
TIMEOUT="${TIMEOUT:-12}"

cd "$ROBOT"
make clean BUILD=build-bench
make DEFS="-DAEVUM_SCHED_BENCH -DBENCH_TASKS=$TASKS" BUILD=build-bench all
mkdir -p "$OUT"

for r in $(seq 1 "$REPS"); do
    timeout "$TIMEOUT" qemu-system-i386 \
        -drive format=raw,file=disk.img \
        -m 512M -smp 1 -accel tcg -nographic -no-reboot \
        > "$OUT/a0-${TASKS}-r${r}.log" 2>&1 || true
done

grep -h "A0_BENCH" "$OUT"/a0-"$TASKS"-r*.log
