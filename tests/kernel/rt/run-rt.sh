#!/usr/bin/env bash
# RT-0b：ring0 10ms 控制任务 + 0/4/8/12 个 ring3 mode 7 后台任务。
# 只在 WSL/Linux 里跑；结果日志写到 tests/kernel/results 下，不提交。
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
ROBOT="$ROOT/src/kernel/host/console/robot"
BG="${RT_BG_TASKS:-0}"
REPS="${REPS:-3}"
TIMEOUT="${TIMEOUT:-300}"
OUT="${1:-$ROOT/tests/kernel/results/2026-11-26/rt-bg$BG}"

case "$BG" in
    0|4|8|12) ;;
    *) echo "RT_BG_TASKS must be 0/4/8/12" >&2; exit 2 ;;
esac

cd "$ROBOT"
make clean BUILD=build-rt
make BUILD=build-rt DEFS="-DAEVUM_RT_BENCH -DRT_BG_TASKS=$BG" all
mkdir -p "$OUT"

for r in $(seq 1 "$REPS"); do
    timeout "$TIMEOUT" qemu-system-i386 \
        -drive format=raw,file=disk.img \
        -m 512M -smp 1 -accel tcg -nographic -no-reboot \
        > "$OUT/rt-${BG}-r${r}.log" 2>&1 || true
    if ! grep -q "RT_STAT bg=$BG periods=1000" "$OUT/rt-${BG}-r${r}.log"; then
        echo "missing final RT_STAT: bg=$BG rep=$r" >&2
        exit 1
    fi
done

grep -h "RT_STAT" "$OUT"/rt-"$BG"-r*.log
