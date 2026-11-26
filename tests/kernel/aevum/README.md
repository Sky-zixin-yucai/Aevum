# Aevum 侧测试入口

Aevum 是裸机内核，没有 Linux 的用户态工具，采集靠内核页面和 QEMU 截图。

## 构建

在 Linux/WSL2 里构建，依赖 `nasm`、支持 `-m32` 的 `gcc`、`binutils`、`make`、`qemu-system-i386`：

```bash
cd src/kernel/host/console/robot
make clean
make all
```

产物：

```text
src/kernel/host/console/robot/kernel.bin
src/kernel/host/console/robot/disk.img
```

当前基线 `A0` 的 `kernel.bin` 是 87,496B（含 loader 现场修复）；每次测试都要记录新的大小和 `__bss_end`，不能只写“通过”。

## 真实 CPU-bound 基准

新增 `run-bench.sh`，在 WSL/Linux 里构建 `AEVUM_SCHED_BENCH` 专用内核并跑 2/4 个 ring3 CPU-bound 任务：

```bash
# 4 任务，3 次
BENCH_TASKS=4 tests/kernel/aevum/run-bench.sh

# 2 任务（优先级 0 和 3）
BENCH_TASKS=2 tests/kernel/aevum/run-bench.sh
```

它走的是：

- 同一段 `user_program_start`，数据页 mode=7；
- mode 7 只自增计数、不 `yield`，由 PIT 抢占；
- bench 内核任务睡眠 500 tick，500 tick 后读 `task_snapshot_process()` 的 `cpu_ticks`；
- 结果建议写到 `/tmp` 或 `results/2026-11-26/`；原始 `.log` 不提交，只保留汇总和截图。

A0 冻结基线（P11.9 前，3 次一致）：

| 用例 | P0 | P1 | P2 | P3 |
| --- | ---: | ---: | ---: | ---: |
| A0 2 任务（P0/P3） | 20.00% | — | — | 80.00% |
| A0 4 任务 | 10.40% | 20.00% | 30.40% | 39.20% |

A1 CFS-lite 实测（P11.9，3 次一致，500 tick）：

| 用例 | P0 | P1 | P2 | P3 |
| --- | ---: | ---: | ---: | ---: |
| A1 2 任务（P0/P3） | 88.80% | — | — | 11.20% |
| A1 4 任务 | 52.80% | 26.40% | 13.60% | 7.20% |

结论：A0 的 `priority + 1` 让 priority 0 份额最低，和 Linux CFS 的权重方向相反；A1
改成 `8 >> priority` 后，2 任务和 Linux CFS 的 `88.67/11.33` 基本重合，4 任务
`52.80/26.40/13.60/7.20` 也贴近 `53.34/26.64/13.25/6.74`。完整对比见
`tests/kernel/results/2026-11-26/aevum-real.md`。


## 启动

Windows 本地可直接用仓库脚本：

```powershell
.\scripts\kernel\run-qemu.ps1
```

更接近测试矩阵的启动方式：

```powershell
qemu-system-i386 `
  -m 32M `
  -drive format=raw,file=src\kernel\host\console\robot\disk.img `
  -display none `
  -monitor unix:/tmp/aevum-mon,server,nowait `
  -serial tcp:127.0.0.1:5555,server,nowait `
  -no-reboot
```

注意：

- Aevum 的 COM1 同时承载二进制状态帧和调试输出，不能直接用 `strings` 抓文本。
- 调度证据要从 `V` / `C` / `ADDRESS` / `SLOT` 页读，配合 QEMU `screendump`。
- 不要用 `-smp 2`；Aevum 当前是单核调度，和 Linux 的 `-smp 1` 对齐。

## 采集页面

- 主页面：`kt 4 s5 a1 OK`，P5.3 压测，`PHYS` 计数。
- `V` 页：进程/线程、ZOMBIE、waiters、VMA、信号、fd。
- `C` 页：`QUANTUM`、`EXP`、`EINTR`、`SLEEP`、`IO`、`WQ`。
- `ADDRESS` / `SLOT` 页：`cpu_ticks`、switch、退出记录。

`cpu_ticks` 是 PIT/IRQ0 实际落在任务上的 tick 数，适合做份额对比；不要用墙钟时间替代。

## 截图

QEMU monitor 用 HMP：

```text
screendump /tmp/aevum.ppm
```

再转 PNG：

```bash
convert -filter point -resize 200% /tmp/aevum.ppm /tmp/aevum-2x.png
```

每个用例至少保留：

```text
results/<日期>/aevum-a0/<case>-main.png
results/<日期>/aevum-a0/<case>-c.png
results/<日期>/aevum-a0/<case>-addr.png
```

## 和 Linux 对齐

- 同样 1 个 vCPU。
- 同样 TCG 加速；如果 Linux 用 KVM，Aevum 结果不能进同一张表。
- 同样任务数量、优先级、周期、CPU 忙等比例。
- 记录 QEMU 版本和 CPU model。
