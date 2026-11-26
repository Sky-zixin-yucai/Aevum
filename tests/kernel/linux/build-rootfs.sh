#!/usr/bin/env bash
# 构建 Linux i386 基准用的最小 initramfs。
# 产物是一个 32 位静态 /init，不依赖宿主机 BusyBox。
# 用法：
#   ./build-rootfs.sh
#   ROOT=~/aevum-test/rootfs-i386 OUT=~/aevum-test/rootfs-i386.cpio.gz ./build-rootfs.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${ROOT:-$HOME/aevum-test/rootfs-i386}"
OUT="${OUT:-$HOME/aevum-test/rootfs-i386.cpio.gz}"

command -v gcc >/dev/null 2>&1 || {
  echo "missing: gcc" >&2
  exit 1
}
command -v cpio >/dev/null 2>&1 || {
  echo "missing: cpio（apt-get install cpio）" >&2
  exit 1
}

rm -rf "$ROOT"
mkdir -p "$ROOT/proc" "$ROOT/sys/fs/cgroup" "$ROOT/dev" "$ROOT/tmp"

echo "== build /init (i386 static) =="
gcc -m32 -static -O2 -s -Wall -Wextra -o "$ROOT/init" "$HERE/bench/bench.c"
chmod 0755 "$ROOT/init"

file "$ROOT/init"
file "$ROOT/init" | grep -q 'ELF 32-bit' || {
  echo "init 不是 32 位 ELF；检查 gcc -m32 / multilib" >&2
  exit 1
}

mkdir -p "$(dirname "$OUT")"
echo "== pack initramfs =="
(
  cd "$ROOT"
  find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > "$OUT"
)

ls -lh "$OUT"
echo "rootfs: $OUT"
