#!/usr/bin/env bash
# 跑 Linux i386 基线矩阵：CFS 2/4/8/12，FIFO 4。
# 需要先 build-i386.sh 和 build-rootfs.sh。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BZIMAGE="${BZIMAGE:-$HOME/aevum-test/out/linux-i386-hz100/bzImage}"
ROOTFS="${AEVUM_TEST_ROOTFS:-$HOME/aevum-test/rootfs-i386.cpio.gz}"
OUT_DIR="${OUT_DIR:-$HOME/aevum-test/results}"
TIMEOUT="${AEVUM_TEST_TIMEOUT:-45}"
REPS="${REPS:-1}"

run_case() {
  local policy="$1"
  local tasks="$2"
  local name="${policy}${tasks}"
  echo "== $name =="
  for rep in $(seq 1 "$REPS"); do
    local log="$OUT_DIR/${name}-r${rep}.log"
    AEVUM_TEST_BENCH="bench_policy=$policy bench_tasks=$tasks bench_ms=5000" \
    AEVUM_TEST_TIMEOUT="$TIMEOUT" \
    BZIMAGE="$BZIMAGE" \
    AEVUM_TEST_ROOTFS="$ROOTFS" \
    OUT="$log" \
    bash "$HERE/run-qemu.sh" > /dev/null 2>&1 || {
      echo "run failed: $name r$rep" >&2
      tail -40 "$log" >&2 || true
      exit 1
    }
    echo "-- run $rep --"
    grep -E '^(BENCH|TASK_RESULT)' "$log"
  done
  echo
}

run_case cfs 2
run_case cfs 4
run_case cfs 8
run_case cfs 12
run_case fifo 4
