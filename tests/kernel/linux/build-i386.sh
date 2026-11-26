#!/usr/bin/env bash
# 构建 Linux i386 基准内核：1 vCPU、HZ=100、无 high-res、无 NO_HZ。
# 用法：
#   LINUX_SRC=~/aevum-test/linux \
#   BUILD_DIR=~/aevum-test/build/linux-i386-hz100 \
#   OUT_DIR=~/aevum-test/out/linux-i386-hz100 \
#   ./build-i386.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LINUX_SRC="${LINUX_SRC:-$HOME/aevum-test/linux}"
BUILD_DIR="${BUILD_DIR:-$HOME/aevum-test/build/linux-i386-hz100}"
OUT_DIR="${OUT_DIR:-$HOME/aevum-test/out/linux-i386-hz100}"
FRAGMENT="$HERE/configs/i386-hz100.fragment"
JOBS="${JOBS:-$(nproc)}"

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "missing: $1" >&2
    exit 1
  }
}

need git
need make
need gcc

# 32 位编译需要 multilib；先做一次最小链接测试。
tmp_c="$(mktemp /tmp/aevum-m32-XXXX.c)"
tmp_o="$(mktemp /tmp/aevum-m32-XXXX)"
printf 'int main(void){return 0;}\n' > "$tmp_c"
if ! gcc -m32 "$tmp_c" -o "$tmp_o" 2>/tmp/aevum-m32.err; then
  echo "gcc -m32 链接失败；先安装 32 位编译依赖：" >&2
  echo "  sudo apt-get install gcc-multilib libc6-dev-i386" >&2
  cat /tmp/aevum-m32.err >&2
  rm -f "$tmp_c" "$tmp_o"
  exit 1
fi
rm -f "$tmp_c" "$tmp_o"

if [ ! -f "$LINUX_SRC/Makefile" ]; then
  echo "source missing, cloning linux v6.18 -> $LINUX_SRC"
  echo "（GitHub 不可达时，可先从 cdn.kernel.org 下载 linux-6.18.tar.xz 并解压到 $LINUX_SRC）"
  git clone --depth 1 --branch v6.18 https://github.com/torvalds/linux.git "$LINUX_SRC"
fi

mkdir -p "$BUILD_DIR" "$OUT_DIR"

echo "== i386_defconfig =="
make -C "$LINUX_SRC" O="$BUILD_DIR" ARCH=x86 i386_defconfig

echo "== merge fragment =="
"$LINUX_SRC/scripts/kconfig/merge_config.sh" -m "$BUILD_DIR/.config" "$FRAGMENT"

# i386_defconfig 的 PREEMPT_DYNAMIC / NO_HZ / HZ_1000 会把 fragment 的
# “n” 重新选回默认，这里用 scripts/config 明确压住，再让 olddefconfig 收敛。
"$LINUX_SRC/scripts/config" --file "$BUILD_DIR/.config" \
  -d PREEMPT_DYNAMIC -d PREEMPT_VOLUNTARY -d PREEMPT -e PREEMPT_NONE \
  -d NO_HZ_IDLE -d NO_HZ_COMMON -d NO_HZ -d HIGH_RES_TIMERS \
  -e HZ_100 -d HZ_250 -d HZ_300 -d HZ_1000 \
  -d SMP --set-val NR_CPUS 1

make -C "$LINUX_SRC" O="$BUILD_DIR" ARCH=x86 olddefconfig

echo "== verify =="
grep -q '^# CONFIG_64BIT is not set' "$BUILD_DIR/.config" || {
  echo "CONFIG_64BIT is not disabled" >&2
  exit 1
}
grep -q '^CONFIG_HZ=100$' "$BUILD_DIR/.config" || {
  echo "CONFIG_HZ is not 100" >&2
  exit 1
}
grep -q '^# CONFIG_SMP is not set' "$BUILD_DIR/.config" || {
  echo "CONFIG_SMP is not disabled" >&2
  exit 1
}

echo "== build bzImage -j$JOBS =="
make -C "$LINUX_SRC" O="$BUILD_DIR" ARCH=x86 -j"$JOBS" bzImage

cp "$BUILD_DIR/arch/x86/boot/bzImage" "$OUT_DIR/bzImage"
cp "$BUILD_DIR/.config" "$OUT_DIR/config"

echo "== done =="
echo "bzImage: $OUT_DIR/bzImage"
echo "config:  $OUT_DIR/config"
file "$OUT_DIR/bzImage"
