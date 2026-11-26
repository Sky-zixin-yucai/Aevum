#!/usr/bin/env bash
# 启动 Linux i386 基准内核并采集串口输出。
# 需要 AEVUM_TEST_ROOTFS 指向 32-bit initramfs（cpio.gz）。
# 用法：
#   BZIMAGE=~/aevum-test/out/linux-i386-hz100/bzImage \
#   AEVUM_TEST_ROOTFS=~/aevum-test/rootfs-i386.cpio.gz \
#   ./run-qemu.sh
set -euo pipefail

QEMU="${QEMU:-qemu-system-i386}"
BZIMAGE="${BZIMAGE:-${AEVUM_TEST_BZIMAGE:-}}"
ROOTFS="${AEVUM_TEST_ROOTFS:-}"
OUT="${OUT:-${AEVUM_TEST_OUT:-./linux-serial.log}}"
APPEND="${AEVUM_TEST_APPEND:-console=ttyS0,115200 panic=-1 loglevel=7}"
BENCH="${AEVUM_TEST_BENCH:-bench_policy=cfs bench_tasks=4 bench_ms=5000}"
MEM="${AEVUM_TEST_MEM:-512M}"
ACCEL="${AEVUM_TEST_ACCEL:-tcg}"
CPU="${AEVUM_TEST_CPU:-}"
TIMEOUT="${AEVUM_TEST_TIMEOUT:-90}"

if [ -z "$BZIMAGE" ] || [ ! -f "$BZIMAGE" ]; then
  echo "BZIMAGE not found; set BZIMAGE to the built bzImage" >&2
  exit 1
fi
if [ -z "$ROOTFS" ] || [ ! -f "$ROOTFS" ]; then
  echo "AEVUM_TEST_ROOTFS not found; build or point to a 32-bit initramfs" >&2
  exit 1
fi
command -v "$QEMU" >/dev/null 2>&1 || {
  echo "missing: $QEMU" >&2
  exit 1
}
command -v timeout >/dev/null 2>&1 || {
  echo "missing: timeout" >&2
  exit 1
}

args=(
  -m "$MEM"
  -smp 1
  -kernel "$BZIMAGE"
  -initrd "$ROOTFS"
  -append "$APPEND $BENCH"
  -nographic
  -no-reboot
)

if [ -n "$ACCEL" ]; then
  args+=(-accel "$ACCEL")
fi
if [ -n "$CPU" ]; then
  args+=(-cpu "$CPU")
fi

mkdir -p "$(dirname "$OUT")"
set -o pipefail
timeout "${TIMEOUT}s" "$QEMU" "${args[@]}" 2>&1 | tee "$OUT"
