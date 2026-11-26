# 调度测试矩阵

这份矩阵先服务“和 Linux 32-bit 对比”，不追求覆盖所有 POSIX 语义。每个用例都要能落到 CSV，不能只留截图。

## 页面预演

正式跑 K0 之前，先用 `arena/index.html` 把同一任务集在 A0 / CFS / FIFO 三种模型下过一遍：

- 只用来确认差异方向、发现指标异常，结果不写进 CSV，也不当 QEMU 证据。
- A0 两任务应看到 `20% : 80%`，CFS 应看到 `88.9% : 11.1%`；如果对不上，先查模型链路。
- FIFO 四任务应看到最高优先级 `100%`、其余 `0%`、最大等待 `1000`；这是控制类的最坏情况。
- 预演通过后再进 K0，避免拿一个方向都不对的模型去和 Linux 比。

## 用例

### K0 回归基线

目的：先确认新测试没有破坏已有内核能力。

| 项 | 要求 |
| --- | --- |
| Aevum | 主页 `kt 4 s5 a1 OK`；P5.3 `round 100/100 ... STRESS OK`、`PHYS 6/255`；P11.7 `EINTR` 计数；P11.8 image3；P12.1 image4 |
| Linux | 同参数下能启动、跑完基准、正常关机 |
| 判定 | 任何一项失败，后续对比结果不采信 |

### K1 两任务公平性

目的：验证权重是否真的变成 CPU 份额。

- 任务：2 个 CPU-bound，priority 0 和 3。
- 时长：500 tick（5 s，`aevum/run-bench.sh` 默认）。
- 采集：每个任务的 `cpu_ticks`，计算实际份额。
- A0 预期（P11.9 前冻结）：权重 `1:4`，理论份额约 `20%:80%`。
- A1 预期（P11.9 起）：权重 `8:1`，理论份额约 `88.9%:11.1%`。
- CFS 预期：Linux 用 cgroup v2 `cpu.weight = 800/400/200/100`，理论份额约 `53.3% / 26.7% / 13.3% / 6.7%`；两任务为 `88.9% / 11.1%`。
- FIFO 预期：高优先级独占，低优先级接近 0%，最大等待接近整个窗口；这是 starvation 上界。
- 判定：实测比例和理论比例的相对误差；A0、A1 和 Linux 分开报，不混算。

### K2 多任务扩展性

目的：看任务数从 1 增到 12 时，调度开销和份额是否崩。

- 任务：1 / 4 / 8 / 12 个 CPU-bound，优先级均匀分布。
- 时长：每个配置 1000 tick。
- 采集：总吞吐、每任务份额、最大等待时间、上下文切换次数。
- 判定：是否有任务 `cpu_ticks=0`；最大等待/平均等待的比值。

### K3 唤醒延迟

目的：测“阻塞任务被唤醒后多久真正上 CPU”。

- 场景：低优先级任务占 CPU；高优先级任务在 pipe 上 `read_block`，由另一个任务写入后唤醒。
- 采集：唤醒 tick 到目标任务首次运行的 tick 差。
- 输出：p50 / p99 / max，单位 tick；同时记录切换次数。
- Linux 对照：CFS 和 `SCHED_FIFO` 各跑一遍；`SCHED_FIFO` 关 RT throttling。

### K4 周期任务抖动

目的：这是机器人控制场景的核心指标，不是通用 OS 的吞吐。

- 场景：周期任务，周期 10 / 20 / 50 tick，执行 1–2 tick。
- 采集：每次实际开始 tick 与理论开始 tick 的偏差。
- 输出：p50 / p99 / max jitter，deadline miss 次数。
- Linux 对照：CFS 与 `SCHED_FIFO`；`SCHED_FIFO` 要关 RT throttling，否则默认 95% 限制会制造 miss。

### K5 睡眠补偿

目的：防止睡很久的任务醒来后一口气追回全部 `vruntime`，把别人饿死。

- 场景：任务 A CPU-bound；任务 B 睡 100 tick 后醒来，再跑 200 tick。
- 采集：B 醒来后的连续运行长度、A 的最长等待、两者份额。
- 判定：B 的补偿上限是否受控；A 是否出现长时间零份额。

### K6 切换与定时器开销

目的：区分“算法差”和“实现开销大”。

- 场景：空转任务 + 固定数量 CPU-bound 任务；分别跑 1000 tick。
- 采集：上下文切换次数、每 tick 的切换率、调度器占用的 tick 比例。
- 对照：Linux `perf sched` / `/proc/schedstat`；Aevum 用现有 switch 计数和 C 页统计。

## 指标定义

| 指标 | 定义 | 单位 |
| --- | --- | --- |
| `cpu_ticks` | PIT/IRQ0 实际落在该任务上的 tick 数 | tick |
| `share` | `cpu_ticks / 总有效 tick` | % |
| `wait_max` | 任务从 READY 到首次 RUNNING 的最长等待 | tick |
| `wake_p50/p99/max` | 阻塞唤醒到首次运行的分位 | tick |
| `jitter_p50/p99/max` | 周期任务实际开始与理论开始的偏差 | tick |
| `deadline_miss` | 实际完成晚于 deadline 的次数 | 次 |
| `switches` | 上下文切换次数 | 次 |
| `switch_rate` | `switches / tick` | 次/tick |

## 结果格式

每个用例一行，字段至少包含：

```text
case,kernel,policy,task_set,tasks,ticks,cpu_ticks,share,wait_max,wake_p50,wake_p99,wake_max,jitter_p50,jitter_p99,jitter_max,deadline_miss,switches,notes
```

Aevum 侧额外记录：

- `kernel.bin` 大小、`__bss_end`、`AEVUM_KERNEL_END` 余量
- QEMU 参数、CPU model、PIT 频率
- 采集页面：C / V / ADDRESS / SLOT
- 截图路径

Linux 侧额外记录：

- `uname -a`、`.config` 里 `CONFIG_HZ`、`CONFIG_PREEMPT*`、`CONFIG_SMP`
- QEMU 参数与加速方式
- `/proc/schedstat`、`/proc/<pid>/sched` 或 `perf sched` 输出

## 采集方式

### Aevum

- `cpu_ticks` 已在 `ADDRESS` / `SLOT` 页展示；截图时同时记录任务 PID/TID。
- `QUANTUM`、`EXP`、`EINTR`、`SLEEP`、`IO`、`WQ` 从 C 页读。
- 线程组与退出记录从 V 页读。
- 每个用例至少截主页面、C 页、对应 ADDRESS/SLOT 页三张图。

### Linux

- 启动参数固定 `-smp 1 -m 512`，与 Aevum 内存不同时要在报告里注明。
- 用 `cyclictest` 测唤醒/周期延迟；用 `perf sched` 或 `/proc/schedstat` 测切换。
- `SCHED_FIFO` 用例先确认 `sched_rt_runtime_us=-1`，再开始跑。

## 报告顺序

1. 先给 K0 回归结果，证明基线可用。
2. 再给 K1/K2 公平性与吞吐。
3. 再给 K3/K4 延迟与抖动。
4. 最后给 K5/K6 解释差异来源。
5. 结论只写“差多少、差在哪、是否值得重设计”，不写“Linux 有所以我们也要有”。
