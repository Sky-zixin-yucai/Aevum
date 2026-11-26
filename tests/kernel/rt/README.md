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

RT-1 的 10ms 模型同时记录两种口径：`RT_ICOUNT=1` 的确定性虚拟时钟隔离 guest 调度路径；默认宿主墙钟暴露 QEMU 进程被宿主调度器打断的真实抖动，用 `run-rt-host.ps1` 固定 0.5ms 定时器分辨率。两种口径都必须满足上表；确定性结果不能替代宿主墙钟。整机暂停样本仍按 `HOST_INVALID` 拒绝，不能删除。

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

RT-1 第一刀再改 `need_resched` 与唤醒抢占；1kHz PIT 已验证会放大 TCG 宿主抖动，不用于 10ms 模型；1ms 伺服另做 APIC/HPET one-shot。

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
5. **RT-1 的第一刀必须切在唤醒抢占**：在 `task_wake_expired()` 或 `task_wake()` 把任务置 READY 后设置 `need_resched`，让 PIT 出口不再因 `tick_defer` 直接返回；同时把 PIT 提到 1kHz、控制任务给 1 tick 片，是第二刀的独立问题。只把 PIT 改 1kHz、仍保留 4 tick 片，最坏延迟仍是 4ms 量级。

### 10.2 结果边界

- QEMU TCG 的 TSC 只用于相对比较；这些数字证明“当前 A1 的调度路径会延迟”，不证明真实硬件上的绝对最坏延迟。
- `p99` 是 10us 量化后的分位值，`max` 是精确 cycle 换算；两者不应混用。
- 0 后台的 30ms 不是“空载异常”，而是 idle 也被 4 tick 片约束的直接结果；RT-1 需要让 idle 在 control 唤醒时立即让出。
- 12 后台的结果单独报告；它已经接近 `PROC_MAX=12` 和 4 tick 片的组合上限，不能拿它替代 4/8 的门槛。

## 11. RT-1 第一刀：唤醒抢占与最小 RT 类（已落地）

第一刀只改“唤醒后能不能立刻重选”，不碰 PIT 频率、不改 `TASK_SLICE_TICKS`、不做 CBS：

- `task_struct` 增加 `int rt`；`task_zero()` 清零；新增 `task_set_rt()`，只给 `rtbench` 控制任务打标。
- 新增静态 `volatile u32 need_resched` 和 `wake_resched_total`。
- 所有把任务置 `TASK_READY` 的唤醒路径统一调用 `task_note_wake()`：`task_wake_expired()`、`task_wake()`、`task_wake_kthread_waiters()`、`wait_queue_wake_one/all()`、`task_wake_process()`。
- `task_note_wake()` 只在当前任务 RUNNING、目标不是当前任务/idle 时置位；条件是“当前是 idle”“目标是 RT 类”或“目标优先级更高”。它只置标记，不直接切栈。
- `task_schedule_irq()` 的 IRQ0 分支在片未尽时检查 `need_resched`：有标记就不递增 `tick_defer_total`、不设置 `tick_defer`，继续走原 `task_pick_next()`；真正早退后才清标记。
- `task_pick_next()` 先收集 RT READY，再收集 fair READY；有 RT 就返回 RT，否则维持原 CFS-lite `vruntime` 选择。RT 任务同样重置 4 tick 片，但它每周期主动阻塞，不会长期占 CPU。
- `RT_STAT` 增加 `wresched=`，记录唤醒侧置位次数。

这是“最小 RT 类”，不是完整 RT 调度：没有 RT 队列、优先级位图、预算/CBS、RT  throttling 或 deadline 检查；控制任务靠 `task_sleep_ticks()` 自己让出。

### 11.1 为什么第一刀不改 1kHz

RT-0b 的根因是 `tick_defer` 早退，不是 PIT 分辨率：只把 PIT 改 1kHz、仍保留 4 tick 片，控制任务还是会被 4ms 片挡住。第一刀先把“唤醒 → 置 READY → 同 tick 重选”打通，保持 100Hz，避免把片长、idle 探针、kthread grace 和大量 100Hz 文档一起卷进来。

1kHz / APIC one-shot 留给第二刀；它解决的是 1ms 级周期和 tick 粒度，不是当前 10ms 周期的根因。

### 11.2 两种测量口径

RT-0b 的 before 是宿主墙钟口径：QEMU 不启用 `-icount`，PIT 和 `rdtsc` 都跟宿主调度走。RT-1 同时保留这个口径，并新增确定性虚拟时钟口径：

```bash
# 确定性：隔离宿主调度，量 guest 内 IRQ/调度路径
RT_BG_TASKS=4 RT_ICOUNT=1 REPS=3 TIMEOUT=60 \
  tests/kernel/rt/run-rt.sh /mnt/d/Aevum/tests/kernel/results/2026-11-26/rt-1-icount-bg4

# 宿主墙钟：保留原来的 QEMU 参数，不加 -icount
RT_BG_TASKS=4 REPS=3 TIMEOUT=60 \
  tests/kernel/rt/run-rt.sh /mnt/d/Aevum/tests/kernel/results/2026-11-26/rt-1-bg4
```

`RT_ICOUNT=1` 追加 `-icount shift=auto,align=off,sleep=off`。它把 guest 时间变成确定性虚拟时间，`cyc_us` 会从约 3,400 降到约 950–1,000，这是虚拟 TSC 频率，不是硬件频率；延迟/抖动只能和同一口径比较。

## 12. RT-1 第一刀结果

### 12.1 确定性虚拟时钟（门槛口径）

三次 1000 周期，表中为三次最坏值；`wresched` 三次均为 1001，`wake_miss=0`、`sleep_fail=0`、`late=0`。

| 后台 | max latency | p99 latency | max jitter | max exec | deadline miss | overrun | switches |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 16us | 10us | 19us | 1.800ms | 0/1000 | 0 | 2003 |
| 4 | 19us | 10us | 185us | 1.800ms | 0/1000 | 0 | 2005 |
| 8 | 22us | 20us | 365us | 1.800ms | 0/1000 | 0 | 2005 |
| 12 | 24us | 20us | 562us | 1.800ms | 0/1000 | 0 | 2005 |

原始三次 `RT_STAT`：

```text
bg=0  lat_max=16/16/16us       p99=10/10/10us       jitter=19/19/19us       exec=1800/1800/1800us  switches=2003
bg=4  lat_max=19/19/19us       p99=10/10/10us       jitter=184/184/185us    exec=1800/1800/1800us  switches=2005
bg=8  lat_max=22/22/22us       p99=20/20/20us       jitter=363/365/363us    exec=1800/1800/1800us  switches=2005
bg=12 lat_max=24/24/24us       p99=20/20/20us       jitter=560/562/562us    exec=1800/1800/1800us  switches=2005
```

**4/8 后台的最大唤醒延迟和抖动都低于 1ms，1000 周期零 miss、零 overrun。** 12 后台也低于 1ms，单独报告。

### 12.2 宿主墙钟（门槛口径）

RT-1 的 10ms 控制模型不靠 1kHz PIT。宿主墙钟用 `run-rt-host.ps1` 把 Windows 定时器分辨率提到 0.5ms 后跑默认 `-icount 0` 的 QEMU；三次最坏值：

| 后台 | max latency | p99 latency | max jitter | max exec | deadline miss | overrun |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 201us | 60us | 705us | 1.951ms | 0/1000 | 0 |
| 8 | 405us | 120us | 820us | 1.849ms | 0/1000 | 0 |
| 12 | 166us | 100us | 703us | 2.579ms | 0/1000 | 1 |

`rt-1-host-bg4-stall` 仍是 `valid=0`：`lat_max=2780us`、`jitter=15931us`、`miss=1`、`exec=9167us`、`late=1`、`wake_miss=1`。这不是内核把控制任务延迟了 15ms，而是整个 QEMU 进程接近 10ms 没拿到宿主 CPU；PIT tick 和 TSC 一起跳，控制任务的 `next_tick` 已经过期。这类样本不参与门槛，但不能删除；`RT_STRICT=1` 遇到 `late>0`、`wake_miss>0` 或 `sleep_fail>0` 直接判 `HOST_INVALID`。

**宿主隔离**：Windows 默认定时器分辨率会把 QEMU 的 10ms PIT 抖动放大到 1.2–1.9ms；`run-rt-host.ps1` 调用 `NtSetTimerResolution(5000)`（0.5ms），在同一个 PowerShell 进程里同步跑 WSL QEMU，退出时恢复。三次 fresh 样本因此稳定在 705/820/703us。若宿主仍偶发整 tick 丢失，`RT_STRICT=1` 会拒绝该样本，必须重跑，不能把它算进三次。

**1kHz PIT 试验不采用**：把 `PIT_HZ` 直接改成 1000 后，确定性 `-icount` 仍正常，但宿主墙钟 4 后台出现 `jitter=9.989ms`、`exec=12.055ms`、`miss=1`；1kHz 周期 IRQ 在 TCG 下比 100Hz 更贵，反而放大宿主抖动。因此 RT-1 的 10ms 模型保留 100Hz PIT；1ms 伺服仍需 APIC/HPET one-shot，不能靠 1kHz PIT 硬顶。

结论：

- **确定性口径**：4/8/12 后台最大延迟 19/22/24us、最大抖动 185/365/562us，1000 周期零 miss、零 overrun。
- **宿主墙钟口径**：4/8 后台最大延迟 201/405us、最大抖动 705/820us，1000 周期零 miss；12 后台单独报告，最大抖动 703us、零 miss、单次 overrun 1。
- `rt-1-host-bg4-stall` 是宿主停顿无效样本，不是 `need_resched` 回归，不需要先“修调度器”再闭合；需要的是宿主隔离和严格样本有效性。

### 12.3 与 RT-0b before 的对比

| 后台 | RT-0b max latency | RT-1 确定性 max latency | RT-0b max jitter | RT-1 确定性 max jitter | RT-0b miss | RT-1 确定性 miss |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 30.526ms | 16us | 30.594ms | 19us | 1000/1000 | 0/1000 |
| 4 | 152.176ms | 19us | 152.281ms | 185us | 1000/1000 | 0/1000 |
| 8 | 313.064ms | 22us | 313.157ms | 365us | 1000/1000 | 0/1000 |
| 12 | 472.393ms | 24us | 472.403ms | 562us | 1000/1000 | 0/1000 |

结论：**唤醒抢占切对了根因。** control 不再等 idle 的剩余片，也不再排在多个 CPU-bound fair 任务后面；它在一个 PIT tick 的 IRQ 出口被选上并恢复。`exec` 保持 1.8ms，deadline 10ms 有足够余量。

## 13. 构建、BSS 与回归

第一刀 clean build：

- 正常 A1：`text=87,734`、`data=232`、`bss=16,892`、`kernel.bin=88,008B`；`__bss_start=0x957E0`、`__bss_end=0x999DC`，距 `AEVUM_KERNEL_END=0x9A000` 余 `0x624`（1,572B）。
- RT 构建（`RT_BG_TASKS=12`）：`text=39,598`、`data=228`、`bss=17,016`、`kernel.bin=39,844B`；`__bss_end=0x8DE38`，距 `0x9A000` 余 `0x11C8`（4,552B）。
- 相对 RT-0b：正常 `bss` +96B、`kernel.bin` +288B；RT `bss` +96B；主要来自 `task_struct.rt` 的 4B × 16 槽、`need_resched`/`wake_resched_total` 8B 和对齐。没有新增动态分配，没有顶到 `AEVUM_KERNEL_END`。

正常 A1 clean build 回归（三次一致）：

```text
4 任务：p0/p1/p2/p3 = 52.80% / 26.40% / 13.60% / 7.20%
2 任务：p0/p1       = 88.80% / 11.20%
```

与 P11.9 A1 基线逐位一致；正常构建没有 RT 任务，`need_resched` 不会被 RT 分支触发。正常内核启动 smoke 无 fault/panic，UI 正常。

测试脚本：

- `tests/kernel/rt/run-rt.sh` 保持默认 `RT_ICOUNT=0`，新增 `RT_ICOUNT=1` 的可选确定性口径；
- `tests/kernel/rt/run-rt-host.ps1` 是 Windows/WSL 宿主入口：先 `NtSetTimerResolution(5000)`，再同步跑 `run-rt.sh`，退出前恢复；
- 最终命令：`pwsh -NoProfile -File tests/kernel/rt/run-rt-host.ps1 -Bg 4 -Reps 3`；确定性口径加 `-Deterministic`；
- 结果 CSV：`tests/kernel/results/2026-11-26/rt-1-final.csv`（ignored，不提交）；
- 原始 QEMU 日志在 `tests/kernel/results/2026-11-26/rt-1-*`（ignored，不提交）。

## 14. RT-2 第一版：CBS 预算账本与 throttle（已落地，硬 2ms 未闭合）

RT-2 给控制任务加 CBS 预算：10ms 周期、2ms 预算、正常工作量 1.8ms。它不替代 RT-1 的唤醒抢占；预算耗尽后只影响 RT 任务自己的继续运行资格，fair 回退保持不变。

### 14.1 契约与 API

- 每任务字段：`rt_budget_cycles`、`rt_budget_left`、`rt_budget_last_tsc`、`rt_throttle_until`、`rt_period_ticks`、`rt_throttled`。
- `task_set_rt_budget(task,budget_cycles,period_ticks)`：只对已标记 `rt` 的任务生效；`period_ticks=0` 按 1 tick 处理。
- `task_rt_budget_begin()`：周期开始补满预算并建立 TSC 起点。
- `task_rt_budget_end()`：控制工作结束结清本段预算，并清 `last_tsc`，避免把工作结束后的截止检查、打印和 sleep 尾部算成控制预算。
- `task_rt_budget_tick(now_tick)`：PIT tick 扫描被 throttle 的任务；`now_tick >= rt_throttle_until` 时补满、清 throttle，并置 `need_resched`。
- `task_rt_budget_expire()`：CBS 到期事件入口；只对当前 RUNNING 的 RT 任务记账。
- `task_rt_budget_overrun_total()` / `task_rt_budget_throttle_total()`：汇总 `budget_overrun` 与 `throttle`。
- `task_pick_next()` 跳过 `rt_throttled` 的 RT 任务；选中 RT 时若 `last_tsc=0` 建立起点。
- `task_note_wake()` 对 RT 任务做周期重置；一次唤醒视为新周期。
- 非 tick 的 yield/block/exit 退出也调用 `task_rt_budget_charge()` 结清当前预算。

编译宏：

| 宏 | 默认 | 说明 |
| --- | ---: | --- |
| `RT_BUDGET` | 0 | 0 保持 RT-1 行为；1 开 CBS 账本 |
| `RT_BUDGET_ONESHOT` | 0 | 1 才启用 PIT 2ms one-shot 硬切；默认关闭 |
| `RT_WORK_US` | 1800 | 正常控制工作量；恶意场景用 3000 |

`RT_STAT start` 增加 `enforce=`、`oneshot=`；`RT_STAT` 汇总增加 `budget_overrun=`、`throttle=`。`overrun` 仍是 `exec > 2ms` 的原始执行口径，`budget_overrun` 是 CBS 账本判定，两者不合并。

### 14.2 tick 级账本：确定性结果

`RT_BUDGET=1`、`RT_BUDGET_ONESHOT=0`、4/8 后台、三次 1000 周期。表中为三次最坏值；`miss`/`budget_overrun`/`throttle` 三次均为 0，`exec` 均为 1800us。

| 后台 | max latency | p99 latency | max jitter | max exec | miss | budget_overrun | throttle | switches |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 38us | 30us | 207us | 1800us | 0/1000 | 0 | 0 | 2005 |
| 8 | 42us | 40us | 389us | 1800us | 0/1000 | 0 | 0 | 2005 |

原始三次：

```text
bg=4 lat_max=19/19/38us p99=10/10/30us jitter=184/184/207us exec=1800/1800/1800us miss=0 budget_overrun=0 throttle=0 switches=2005
bg=8 lat_max=42/42/42us p99=40/40/40us jitter=389/389/389us exec=1800/1800/1800us miss=0 budget_overrun=0 throttle=0 switches=2005
```

结论：正常 1.8ms 控制工作量在 2ms 预算内，tick 级账本不产生 throttle；RT-1 的唤醒延迟和切换次数保持不变。

### 14.3 恶意 3ms 控制：账本能记账，但不能 2ms 硬切

`RT_WORK_US=3000`、`RT_BUDGET=1`、`RT_BUDGET_ONESHOT=0`、4 后台一次：

```text
RT_STAT bg=4 periods=1000 lat_n=1000 lat_max_us=19 lat_p99_us=10 jitter_max_us=184 exec_max_us=3000 miss=0 overrun=1000 budget_overrun=1000 throttle=1000 wake_miss=0 sleep_fail=0 late=0 wresched=1001 switches=2005 cyc_us=982
```

这说明账本把 3ms 工作量逐周期判成 `budget_overrun` 并 throttle，但它是在 tick/工作结束边界发现，**没有在 2ms 处切走控制任务**；`exec_max` 仍是 3000us。tick 级账本不能作为“2ms 硬截断”的证据，只能作为预算记账和下一周期补满的证据。

### 14.4 PIT one-shot 2ms 硬切试验：确定性可用，宿主不稳定

`RT_BUDGET_ONESHOT=1` 时，`interrupt_arm_rt_budget()` 把 PIT 通道 0 的 100Hz 周期方波切成 2ms one-shot；预算 IRQ 不推进 tick，只调 `task_rt_budget_expire()` 并强制重选，随后装 `rest_us` 到下一个 10ms tick，tick 到达恢复周期模式。

确定性 4 后台正常 1.8ms 一次：

```text
RT_STAT bg=4 periods=1000 lat_n=1000 lat_max_us=38 lat_p99_us=30 jitter_max_us=215 exec_max_us=1800 miss=0 overrun=0 budget_overrun=0 throttle=0 wake_miss=0 sleep_fail=0 late=0 wresched=1001 switches=2470 cyc_us=982
```

预算中断把切换从 2005 提到 2470，但零 miss、零预算超限。3ms 恶意场景在 one-shot 下出现 `lat_n=1`、`wake_miss=999`、`late=999`、`exec=10204us`：控制任务被 2ms 预算中断后跨周期恢复，测试的 `next_tick` 相位和 `start_tsc` 口径没有适配“被预算切走再恢复”的工作，不能作为有效验收样本。该场景需要单独的 `RT_BAD` 模式重新设计。

宿主墙钟 one-shot 4 后台：rep1 有效 `lat_max=109us`、`jitter=851us`、`miss=0`、`budget_overrun=8`；rep2 `jitter=12286us`、`exec=11601us`、`miss=2`、`late=1`，被 `HOST_INVALID` 拒绝。one-shot 在宿主 TCG 下会放大 tick 相位抖动并产生预算误报，因此默认关闭，不作为 RT-2 宿主门槛。

### 14.5 宿主墙钟 tick 级账本

`run-rt-host.ps1 -Budget` 跑默认 `RT_ICOUNT=0`，4 后台三次：

| 样本 | max latency | p99 | max jitter | max exec | miss | overrun | budget_overrun | throttle | switches |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 187us | 80us | 594us | 1833us | 0 | 0 | 6 | 7 | 2011 |
| 2 | 216us | 40us | 515us | 1895us | 0 | 0 | 7 | 8 | 2013 |
| 3 | 285us | 70us | 1101us | 1883us | 0 | 0 | 7 | 7 | 2011 |

4 后台保持零 miss；最大延迟 285us。但 `budget_overrun` 出现 6–7 次，且第 3 次抖动 1101us，略超 1ms。这些不是控制任务真的跑了 2ms，而是宿主 TCG 停顿把 TSC 墙钟时间计入预算；tick 级账本在宿主墙钟下会误报，不能要求零 `budget_overrun`。

8 后台没有拿到同一轮三次干净样本；有效样本分别来自三次重跑：

```text
bg=8 lat_max=348us jitter=8463us miss=0 overrun=0 budget_overrun=7 throttle=7 switches=2011
bg=8 lat_max=547us jitter=3818us miss=0 overrun=1 budget_overrun=9 throttle=9 switches=2013
bg=8 lat_max=228us jitter=1508us miss=0 overrun=3 budget_overrun=10 throttle=10 switches=2013
```

三次都是零 miss，但抖动 1.5–8.4ms，明显是宿主停顿；另外两次 `HOST_INVALID`（`wake_miss=1`、`late=1`）未计入。RT-2 的宿主 8 后台只能作为“零 miss”观察，不能作为干净的三次门槛。

### 14.6 结论与边界

- 确定性口径：4/8 后台正常 1.8ms 控制零 miss、零 `budget_overrun`、零 throttle；CBS 账本和 throttle 选择本身能工作。
- 宿主墙钟：4 后台零 miss、最大延迟 285us；8 后台零 miss，但只有单次有效样本且抖动毫秒级。宿主 `budget_overrun` 是停顿误报，不应当作控制任务超预算。
- **硬 2ms 截断未闭合**：tick 级账本只能在 10ms tick/工作结束发现超预算；PIT one-shot 在确定性口径能 2ms 切，但宿主不稳定，3ms 恶意场景的测试口径也要重做。
- 下一步要么接 APIC/HPET one-shot，把 2ms 预算做成独立高精度定时器；要么明确 RT-2 只做“预算记账 + 下一周期 throttle”，不承诺 2ms 硬切。
