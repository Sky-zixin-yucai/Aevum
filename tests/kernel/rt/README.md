# RT-0a：实时控制模型与度量契约

本文件只定义“实时性怎么量、什么算通过”，不改调度器、不加 CBS、不加高精度定时器。真正的观测代码在 RT-0b，真正的抢占改动在 RT-1。

## 1. 默认控制模型

先用一个可复现的软实时目标把链路打通；如果目标控制周期是 1ms，参数要另立一组，不能拿 10ms 的结论外推。

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| 控制周期 `P` | 10ms | 100Hz 的 1 tick；1kHz 的 10 tick |
| 每周期预算 `Q` | 2ms | 控制最多连续占用 2ms，占 20% |
| 截止时间 `D` | 10ms | 从预定 release 到控制执行结束 |
| 系统利用率 `U` | 0.2 | 单控制任务；多控制任务按 `ΣQ/P ≤ 0.8` 预留 |
| 周期数 | 1000 | 约 10s，重复 3 次 |
| 后台负载 | 0 / 4 / 8 / 12 | 全部是 ring3 mode 7 CPU-bound |

控制任务约定：

- 运行在 ring0，不经过用户态 syscall；
- 不读 console、不写 VGA、不在周期内做 UART 输出；
- 不动态分配，不触发缺页，栈和缓冲区静态预留；
- 周期内做固定工作负载，结束后 `task_sleep_ticks()` 到下一个 release；
- 不参与 fair 类的权重竞争；RT-0 只标记它是测量对象，RT-1 再把它纳入 RT 类。

后台任务约定：

- 同一段 `user_program_start`，数据页 mode=7；
- 只自增计数、不 `yield`，由 PIT 抢占；
- 优先级均匀分布，避免全都压在同一权重；
- 0 后台任务也必须跑一次，作为空载基线。

## 2. 时间基准

RT-0a 允许两级时间：

- `tick`：PIT 计数，100Hz 基线下 1 tick = 10ms，1kHz 后 1 tick = 1ms；用于 release/deadline 的功能判定。
- `tsc`：`rdtsc` 的 64 位 cycle 计数，用于亚 tick 延迟和抖动；启动时用 PIT 校准 TSC 频率。

口径：

- `release_tsc`：定时器到期、把控制任务从 BLOCKED 改 READY 的那次唤醒时间；
- `start_tsc`：控制任务第一条测量指令的时间；
- `end_tsc`：周期工作负载结束的时间；
- `latency = start_tsc - release_tsc`，单位先记 cycle，再换算 us；
- `jitter = (start_tsc - prev_start_tsc) - P_cycles`；
- `exec = end_tsc - start_tsc`；
- `deadline_miss`：`end_tsc` 超过预定 release + `D` 的周期数；
- `overrun`：`exec > Q` 的周期数。

注意：

- 这里的 `release_tsc` 是“唤醒后”的时间，不含定时器 IRQ 进入前的关中断延迟；IRQ-off 最大延迟是 RT-3 的指标。
- QEMU TCG 的 TSC 只能做相对比较，不能当硬件实时证明；最终硬实时验收必须在真实硬件上重跑。
- 不在控制周期内打印；所有统计只写静态内存，周期结束后统一输出一行 `RT_STAT`。

## 3. 指标与采样规则

每个周期记录一条静态记录，至少包含：

| 字段 | 含义 |
| --- | --- |
| `period_index` | 第几个周期 |
| `release_tick` / `release_tsc` | 预定/实际唤醒点 |
| `start_tsc` / `end_tsc` | 控制开始/结束 |
| `latency_cycles` | 唤醒到开始 |
| `exec_cycles` | 控制执行时长 |
| `jitter_cycles` | 与上一周期开始点的偏差 |
| `miss` | 本周期是否超过 deadline |
| `overrun` | 本周期是否超过预算 |

汇总只报：

- `max_latency`；
- `p50_latency` / `p99_latency`；
- `max_jitter`；
- `max_exec`；
- `deadline_miss_count`；
- `overrun_count`；
- `context_switches`；
- `background_tasks`。

规则：

- 每个配置至少 3 次，报告最大值和 p99；不能只报平均值，也不能挑最好的一次；
- 先跑空载，再跑 4/8/12 后台；空载用于区分调度延迟和负载干扰；
- 先跑当前 A1（100Hz、4 tick 片）作为 before，再跑 RT-1（1kHz、唤醒抢占）作为 after；
- 同一个 QEMU 版本、同一 CPU model、1 vCPU、TCG、512M；QEMU 参数写进结果。

## 4. RT-0 基线不设通过线

RT-0 只建立 before 数字，不做“通过/失败”判断。当前 A1 预期会出现几十毫秒级最坏唤醒延迟，这是后续 RT-1 的对照，不是 bug 结论。

需要记录的可疑现象：

- 空载下控制仍然出现超过 1 tick 的唤醒延迟；
- 4 后台时最大延迟远大于 8 后台，说明存在锁/阻塞而不是调度；
- `end_tsc - start_tsc` 抖动很大，说明控制路径本身有缺页、FPU、console 或长循环；
- `deadline_miss` 集中在某些周期，说明不是均匀延迟而是周期性长尾。

## 5. RT-1 的工程门槛

RT-1 只要求先过第一关，不宣称硬实时：

| 条件 | 门槛 |
| --- | --- |
| 4 后台任务 | 最大唤醒延迟 ≤ 1ms |
| 8 后台任务 | 最大唤醒延迟 ≤ 1ms |
| 周期抖动 | 最大 ≤ 1ms |
| 1000 周期 | deadline miss = 0 |
| 12 后台任务 | 单独报告，不作为 4/8 门槛的替代 |

如果 4/8 都过不了，不进入 RT-2 的 CBS；先查 IRQ-off、FPU、CR3、`cli` 和阻塞路径。

## 6. 结果格式

建议 CSV 表头：

```text
case,reps,period_us,budget_us,deadline_us,background_tasks,hz,slice_ticks,max_latency_us,p50_latency_us,p99_latency_us,max_jitter_us,max_exec_us,deadline_miss,overrun,context_switches,source
```

结果文件放 `tests/kernel/results/<date>/rt-*.csv` 和 `.md`；原始串口日志按现有清理策略不提交。

## 7. RT-0a 不做什么

- 不改 `task.c` 的权重、`min_vruntime`、`TASK_SLICE_TICKS`；
- 不加 `need_resched`、RT 类、CBS、优先级继承；
- 不把 PIT 改成 1kHz，也不接 APIC；
- 不声称“已经实时”，只交付尺子和基线口径。

## 8. RT-0b 的落点

RT-0a 定稿后，RT-0b 只做观测：

- `AEVUM_RT_BENCH` 专用构建；
- ring0 周期控制任务 + mode 7 后台任务；
- `rdtsc` 时间戳与静态统计；
- UART 输出 `RT_STAT`；
- 当前 A1 的 0/4/8/12 后台基线。

RT-1 再改 `need_resched`、唤醒抢占、1kHz 和 control 的 1 tick 片。
