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
- `tsc`：`rdtsc` 的 cycle 计数，用于亚 tick 延迟和抖动；启动时用 PIT 校准 TSC 频率。RT-0b 的实际实现只读低 32 位，10ms 级差值不会回绕；64 位完整计数留给需要长基线的后续版本。

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

## 9. RT-0b 实现（已落地）

新增/改动：

- `app/rtbench/rtbench.c`、`include/app/rtbench/rtbench.h`：控制任务、后台任务、TSC 校准、统计与 `RT_STAT`。
- `include/arch/x86/tsc.h`：`tsc_read32()`，只读 `rdtsc` 低 32 位；10ms 周期的差值远小于 2^32。
- `include/arch/x86/interrupt.h`、`arch/x86/cpu/interrupt/interrupt.c`：`interrupt_tick_hz()` 导出当前 PIT 频率，用于 TSC 校准。
- `include/kernel/task.h`、`kernel/task/task.c`：`task_rt_probe_set/clear/wake_tsc`；`task_wake_expired()` 把探针任务从 BLOCKED 改 READY 时记录 `release_tsc`，不改变调度决策。
- `kernel/main/main.c`：新增 `AEVUM_RT_BENCH` 分支，跳过 arm app，启动 `rtbench_start()`。
- `Makefile`：`SRCS` 加入 `app/rtbench/rtbench.c`；正常构建里该源文件被 `#ifdef AEVUM_RT_BENCH` 编译为空。
- `tests/kernel/rt/run-rt.sh`：按 0/4/8/12 构建并跑 3 次，抓取 `RT_STAT`。

控制任务仍走 fair 类，但只作为观测对象；RT-0b 不改 `min_vruntime`、`TASK_SLICE_TICKS`、`need_resched`，也不把 PIT 改 1kHz。

### 9.1 构建与运行

```bash
# 0 后台
RT_BG_TASKS=0 REPS=3 TIMEOUT=70 tests/kernel/rt/run-rt.sh
# 4 后台
RT_BG_TASKS=4 REPS=3 TIMEOUT=180 tests/kernel/rt/run-rt.sh
# 8 后台
RT_BG_TASKS=8 REPS=3 TIMEOUT=240 tests/kernel/rt/run-rt.sh
# 12 后台
RT_BG_TASKS=12 REPS=3 TIMEOUT=300 tests/kernel/rt/run-rt.sh
```

等价的手工构建：

```bash
cd src/kernel/host/console/robot
make clean BUILD=build-rt
make BUILD=build-rt DEFS="-DAEVUM_RT_BENCH -DRT_BG_TASKS=4" all
qemu-system-i386 -drive format=raw,file=disk.img \
  -m 512M -smp 1 -accel tcg -nographic -no-reboot
```

QEMU 11.1.2，TCG，1 vCPU，512M，PIT 100Hz；`-nographic` 抓串口。每次 1000 周期，约 10s 客机时间；TCG 下实际 wall time 明显更长，12 后台一次接近 5 分钟。

### 9.2 采样口径的落地细节

- `release_tsc` 在 `task_wake_expired()` 里取，早于本轮调度选择；`start_tsc` 在控制任务恢复后取。因此 `latency` 包含“已 READY 但还没被选上”的等待，不只是 IRQ 入口延迟。
- `deadline_miss` 按 `end_tsc - release_tsc > D_cycles` 计；`overrun` 按 `exec_cycles > Q_cycles` 计。
- `jitter` 按相邻两次 `start_tsc` 的间隔减 `P_cycles` 计；首个周期没有前值，不参与。
- 控制工作量固定为 `RT_WORK_US=1800us`，预算仍是 `2000us`，留 200us 给进入/退出与 TSC 读取开销；`RT_STAT start` 会打印 `work_us=1800`。
- 延迟分位数组用 `u16` 存 **10us 单位**，1000 项共 2000B，覆盖 655ms；`lat_max_us` 由精确 cycle 最大值换算，不受分位数组截断影响。
- 0 后台时创建独立 idle 任务并 `task_set_idle_task()`，否则 control 睡下后没有 READY 任务；12 后台时 control + idle + 12 个用户任务 = 14 个任务槽，12 个用户进程正好占满 `PROC_MAX=12`。
- 12 后台构建只用于单独报告，不作为 4/8 门槛的替代。

### 9.3 体积与 BSS

- 正常 A1 clean build：`text=87,455`、`data=232`、`bss=16,796`、`kernel.bin=87,720B`；`__bss_start=0x956C0`、`__bss_end=0x9985C`。
- RT 构建：`bss=16,920`，`__bss_end=0x8D998`；`kernel.bin` 约 38.8–39.5KB。RT 构建跳过 arm app，链接器回收了大部分 UI 静态数据，所以 BSS 没有因为 2KB 延迟数组顶到上限。
- 正常构建没有引入 RT 数组；`app/rtbench/rtbench.c` 在非 RT 构建里为空。

## 10. RT-0b 结果：A1 before

三组 1000 周期；表中为三次中的最坏值，`miss`/`overrun`/`switches` 三次一致（`overrun` 另有单次离群，见脚注）。

| 后台 | max latency | p99 latency | max jitter | max exec | deadline miss | overrun | switches |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 30.526ms | 30.360ms | 30.594ms | 1.943ms | 1000/1000 | 0 | 2003 |
| 4 | 152.176ms | 152.030ms | 152.281ms | 2.000ms | 1000/1000 | 1/3000 | 2881 |
| 8 | 313.064ms | 312.930ms | 313.157ms | 2.511ms | 1000/1000 | 1/3000 | 4760 |
| 12 | 472.393ms | 472.270ms | 472.403ms | 2.154ms | 1000/1000 | 2/3000 | 6639 |

三次原始 `RT_STAT`：

```text
bg=0  lat_max=30494/30459/30526us  p99=30260/30330/30360us  jitter=30488/30594/30482us  exec=1889/1876/1943us  switches=2003
bg=4  lat_max=152176/152141/151585us p99=152030/151980/151530us jitter=152281/152230/151740us exec=2000/1993/1960us switches=2881
bg=8  lat_max=312415/313064/312363us p99=312220/312930/312260us jitter=312472/313157/312379us exec=1940/1925/2511us switches=4760
bg=12 lat_max=472393/472369/471914us p99=472160/472270/471790us jitter=472374/472403/471920us exec=2154/1948/1914us switches=6639
```

### 10.1 关键结论

1. **A1 的唤醒延迟不是几十微秒，而是几十到几百毫秒。** 0 后台就已经是约 30.5ms，正好是 3 个 10ms tick；4/8/12 后台分别约 152/313/472ms。
2. **根因不是 PIT 100Hz 的分辨率，而是 P11.5 的 4 tick 片延迟重选。** `task_wake_expired()` 只把 control 改 READY；`task_schedule_irq()` 在 `slice_left > 0` 时直接返回当前帧，所以 control 要等当前任务（0 后台时是 idle）的剩余片用完才可能被选上。0 后台的 3 tick 就是 idle 剩余片；4/8/12 则叠加了 fair 队列里多个 CPU-bound 任务的片和 vruntime 次序。
3. **1000 周期全部 deadline miss。** 控制预算 2ms、截止 10ms，但唤醒到开始已经超过截止；`exec` 本身约 1.9–2.0ms，不是主要矛盾。
4. **切换次数随后台线性上升**（2003 / 2881 / 4760 / 6639），说明控制任务确实被调度到，只是太晚；这不是丢失唤醒或 `sleep_fail`。三次 `wake_miss=0`、`sleep_fail=0`。
5. **RT-1 的第一刀必须切在唤醒抢占**：在 `task_wake_expired()` 或 `task_wake()` 把任务置 READY 后设置 `need_resched`，让 PIT 出口不再因 `tick_defer` 直接返回；同时把 PIT 提到 1kHz，控制任务给 1 tick 片。否则只把 PIT 改 1kHz，仍然会被 4 tick 片挡住，最坏延迟仍是 4ms 量级。

### 10.2 结果边界

- QEMU TCG 的 TSC 只用于相对比较；这些数字证明“当前 A1 的调度路径会延迟”，不证明真实硬件上的绝对最坏延迟。
- `p99` 是 10us 量化后的分位值，`max` 是精确 cycle 换算；两者不应混用。
- 0 后台的 30ms 不是“空载异常”，而是 idle 也被 4 tick 片约束的直接结果；RT-1 需要让 idle 在 control 唤醒时立即让出。
- 12 后台的结果单独报告；它已经接近 `PROC_MAX=12` 和 4 tick 片的组合上限，不能拿它替代 4/8 的门槛。
