# 内核调度测试

这是 `tests/` 下的二级目录，只放裸机内核的测试与对比，不放普通 Python 单测。

## 目标

进程调度不跳过。P12 的文件分支冻结在 P12.1；这里先把调度测试的基线、模型和 Linux 对照组搭起来，再决定是修现有调度器，还是重设计。

测试要回答的不是“Linux 有没有 CFS”，而是：

- Aevum 当前调度在公平性、吞吐、唤醒延迟、周期抖动上是什么水平；
- Linux 32-bit 的 CFS 和 `SCHED_FIFO` 分别比 Aevum 强多少；
- 强在算法、时间片、定时器粒度，还是只强在 Linux 的通用生态；
- 如果重设计，目标指标能不能被模型预测，并被 QEMU 实测复现。

## 目录

```text
tests/kernel/
├── README.md                 本文件
├── WORKFLOW.md               测试矩阵、指标定义、结果格式
├── aevum/                    Aevum 侧启动、采集与 CPU-bound 基准
│   ├── README.md
│   └── run-bench.sh
├── linux/                    Linux i386 构建与 QEMU 启动
│   ├── build-i386.sh
│   ├── run-qemu.sh
│   └── configs/i386-hz100.fragment
├── model/                    任务集与离线调度模型
│   ├── README.md
│   ├── sim.py
│   └── task_sets/basic.json
├── arena/                    引擎驱动的调度基准场
│   ├── index.html
│   ├── arena.js
│   └── README.md
└── results/                  本地结果，不入库
```

Linux 源码浅克隆在仓库根的排除目录：

```text
D:\Aevum\.excluded\linux      v6.18，depth=1，仅作参考
```

`.excluded/` 已在 `.gitignore` 里；不要把它移进 `tests/`，也不要提交。

## 模型和实测的区别

- `model/` 和 `arena/` 是**预演**：用离散 tick 近似调度器，帮助先确认方向、发现指标异常，不当作 QEMU 证据。
- `linux/` 的 `bench` 和 QEMU 串口输出是**实测**：真实内核、真实定时器、真实上下文切换。
- A0 已补 `aevum/run-bench.sh`：用 mode 7 的 ring3 CPU-bound 任务读真实 `cpu_ticks`；模型数字仍不能直接当 A0 实测。

## 公平条件

对比必须满足这些条件，否则数字不能放在同一张表里：

- 同一个 QEMU 版本、同一个 CPU model、1 个 vCPU、同样内存。
- 同一个加速方式。Aevum 用 TCG，Linux 也必须 TCG；不能一边 KVM 一边 TCG。
- Linux 使用 i386 32-bit 内核，`CONFIG_SMP=n`、`CONFIG_NR_CPUS=1`。
- 基准 Linux 使用 `CONFIG_HZ=100`、`CONFIG_HIGH_RES_TIMERS=n`、`CONFIG_NO_HZ_IDLE=n`，和 Aevum 的 100Hz PIT 对齐。
- 另建一组 `CONFIG_HZ=1000` + high-res 的 Linux，只作为“Linux 最佳状态”上界，不混进基准列。
- Linux 的 `SCHED_FIFO` 测试要关闭 RT throttling；否则默认 95% 上限会污染结果。
- 工作负载语义等价。Aevum 没有 POSIX，就写对应的用户镜像；Linux 用同样的任务数量、周期、CPU 忙等比例、pipe 唤醒路径。
- 每个用例至少跑 3 次，报告中位数和最大值，不挑最好的一次。

## 基线

当前 Aevum 基线记为 `A0`：

- 32-bit x86 裸机内核，100Hz PIT；
- 固定 4 tick 时间片；
- `priority 0..3`，当前代码 `task_weight = priority + 1`；
- 加权 `vruntime`，选择 READY 中最小值；
- 无 `min_vruntime`、无动态片长、无唤醒抢占。

Linux 基线记为 `L100`：

- i386，`HZ=100`，`PREEMPT_NONE`，1 vCPU；
- CFS 默认策略，另跑 `SCHED_FIFO` 作为控制类对照。

可选上界记为 `L1000`：

- i386，`HZ=1000`，high-res timers，1 vCPU；
- 只用于回答“如果 Linux 用更细定时器，能好多少”。

## 工作流

1. **冻结 A0**：先整理现有 P11.5/P11.6/P11.8/P12.1 的 QEMU 证据，作为回归基线。
2. **建模**：用 `model/task_sets/` 的任务集跑 `model/sim.py`，先得到理论份额和等待时间。
3. **页面预演**：用 `arena/index.html` 把模型结果画成时间轴，确认 A0/CFS/FIFO 的差异方向，再进 QEMU。
4. **Linux 基线**：在 Linux 环境用 `linux/build-i386.sh` 构建 `L100`，用 `linux/run-qemu.sh` 跑同一组用例。
5. **Aevum 基线**：用 `aevum/run-bench.sh` 跑 A0 的 2/4 任务 CPU-bound 份额；交互页证据仍按 `aevum/README.md` 采集 C/V/ADDRESS/SLOT。
6. **对照矩阵**：按 `WORKFLOW.md` 的 K0–K6 跑，结果落到 `results/<日期>/<内核>/<用例>.csv`。
7. **结论**：先写“差多少、差在哪”，再决定是否重设计；不先写实现再补测试。

## 结果口径

所有结果必须带：

- 内核与策略：`aevum-a0` / `linux-cfs` / `linux-fifo` / `linux-cfs-hz1000`
- QEMU 参数与 CPU model
- 任务集文件、任务数量、优先级/权重
- 运行 tick 数或墙钟时间
- p50 / p99 / max，不能只给平均值
- 是否出现 deadline miss、饥饿、lost wake

结果文件不入库；只提交测试脚本、任务集和结论摘要。

## 不做

- 不把 Linux 的完整测试框架搬进来；
- 不跑 SMP、NUMA、cgroup、PELT；
- 不用“综合分”掩盖延迟和吞吐的差异；
- 不因为 Linux 有 CFS 就默认 Aevum 也要有 CFS。
