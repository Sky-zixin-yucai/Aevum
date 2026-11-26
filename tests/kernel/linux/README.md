# Linux 32-bit 对照组

这里只做一件事：构建一个和 Aevum 尽量同条件的 i386 Linux，用来跑 CFS 和 `SCHED_FIFO` 基线。

## 为什么是 i386 + HZ=100

- Aevum 是 32 位 x86，Linux 也用 `i386`，避免 64 位内核的调度路径差异。
- Aevum 的 PIT 是 100Hz，基准 Linux 也设 `CONFIG_HZ=100`。
- 关 `HIGH_RES_TIMERS`、`NO_HZ_IDLE`，避免 Linux 用高精度定时器把 Aevum 的 10ms 粒度比下去。
- `PREEMPT_NONE` 对应 Aevum 当前“内核不可抢占、用户态靠 PIT 抢占”的形态。
- 另建 `HZ=1000` 的 `L1000` 只作为 Linux 上界，不混进基准表。

## 源码

仓库根的排除目录里已经有一份浅克隆：

```text
D:\Aevum\.excluded\linux      v6.18, depth=1
```

Windows 文件系统不区分大小写，克隆时 Linux 源码里的大小写同名文件会碰撞（例如 `xt_MARK.c` / `xt_mark.c`）。所以：

- Windows 里这份只用于阅读和查代码；
- 真正构建请在 Linux 环境重新 clone，或使用 Linux 文件系统上的副本；
- `build-i386.sh` 在 `LINUX_SRC/Makefile` 不存在时才尝试浅克隆；GitHub 不通时可用 kernel.org 的源码包：

```bash
cd ~/aevum-test
curl -L -O https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.tar.xz
tar -xf linux-6.18.tar.xz
mv linux-6.18 linux
```

然后照常运行构建脚本。

## 构建

Linux 主机需要：

```bash
sudo apt-get install build-essential gcc-multilib libc6-dev-i386 flex bison \
  libssl-dev libelf-dev bc git
```

然后：

```bash
cd tests/kernel/linux
LINUX_SRC=~/aevum-test/linux \
BUILD_DIR=~/aevum-test/build/linux-i386-hz100 \
OUT_DIR=~/aevum-test/out/linux-i386-hz100 \
./build-i386.sh
```

产物：

```text
out/linux-i386-hz100/bzImage
out/linux-i386-hz100/config
```

脚本会检查：

```text
CONFIG_64BIT is not set
CONFIG_SMP is not set
CONFIG_HZ=100
```

## 32-bit rootfs

`run-qemu.sh` 需要一个 i386 initramfs：

```bash
ROOT=~/aevum-test/rootfs-i386 \
OUT=~/aevum-test/rootfs-i386.cpio.gz \
./build-rootfs.sh
```

最小做法是：

- 用 `bench/bench.c` 编译一个 `gcc -m32 -static` 的 `/init`；
- initramfs 只放 `/init`、`/proc`、`/sys/fs/cgroup`、`/dev`、`/tmp`；
- 不用发行版完整 rootfs，避免后台任务污染调度结果。

`bench` 作为 PID 1 运行，自己 mount proc/sys/cgroup，跑完 CPU-bound 任务后关机。CFS 用 cgroup v2 `cpu.weight` 设 `800/400/200/100`（8:4:2:1）；cgroup 不可用时退回 nice。

## 启动

```bash
BZIMAGE=~/aevum-test/out/linux-i386-hz100/bzImage \
AEVUM_TEST_ROOTFS=~/aevum-test/rootfs-i386.cpio.gz \
AEVUM_TEST_BENCH='bench_policy=cfs bench_tasks=2 bench_ms=5000' \
OUT=~/aevum-test/results/linux-cfs-hz100/serial.log \
./run-qemu.sh
```

`run-qemu.sh` 默认参数：

```text
-m 512M -smp 1 -accel tcg -nographic -no-reboot
```

要和 Aevum 对齐时，不要给 Linux 单独开 KVM；两边都用 TCG。内存可以不同，但必须在报告里写明。

## CFS 与 SCHED_FIFO

同一 rootfs 里跑两组：

```bash
# CFS
AEVUM_TEST_BENCH='bench_policy=cfs bench_tasks=4 bench_ms=5000' \
  ./run-qemu.sh

# FIFO
AEVUM_TEST_BENCH='bench_policy=fifo bench_tasks=4 bench_ms=5000' \
  ./run-qemu.sh
```

`SCHED_FIFO` 是机器人控制任务在 Linux 上的真正对手；CFS 是后台任务对手。两组结果分开报，不能合成一个总分。

## 已跑通

2026-11-25 在 WSL2 Kali + `qemu-system-i386 11.1.2` 上实际编译并运行：

- `bzImage` 11M，`v6.18.0` i386；
- `CONFIG_HZ=100`、`CONFIG_SMP=n`、`CONFIG_PREEMPT_NONE=y`、无 high-res、无 NO_HZ；
- cgroup v2 `cpu.weight=800/400/200/100`，4 任务 CFS 实测约 `53.34% / 26.64% / 13.25% / 6.74%`；
- `SCHED_FIFO` 4 任务实测最高优先级 `99.97%`，其余接近 0，最大等待 5000ms。

结果和原始串口日志在 `results/2026-11-25/linux-i386-hz100/`，该目录不入库。

## 结果

每个用例至少记录：

- `uname -a`
- `zcat /proc/config.gz | grep -E 'CONFIG_HZ|CONFIG_PREEMPT|CONFIG_SMP'`
- `/proc/schedstat`
- `cyclictest` 或自写 benchmark 的 p50/p99/max
- QEMU 参数与加速方式

结果放 `tests/kernel/results/linux-*`，不入库。
