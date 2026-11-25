# P6.0 运行速度基线

本文件只记录当前 `P6.0` clean 构建在 QEMU 里的实测值。它不是硬件实时性报告，也不能直接外推为未来机器人控制器的抖动指标。

## 1. 构建设置

- 源码目录：`D:\Aevum\src\kernel\host\console\robot`
- 构建：WSL Debian，`gcc 16.2.0-3`、`ld 2.46`、`nasm`，`make clean && make kernel.bin && make disk.img`
- QEMU：`qemu-system-i386` 11.1.0，`-m 32M`，`-display none`，TCG（未使用 KVM）
- 镜像：`disk.img=1,474,560` 字节
- 内核：`kernel.bin=64,488` 字节，上限 65,536，余量 1,048 字节
- `boot.bin=512` 字节
- SHA256：kernel `48b25c3f...`，boot `d9a70ce2...`，disk `bbcc9235...`
- BSS 结束：`__bss_end=0x9387C`，`AEVUM_KERNEL_END=0x94000`

## 2. 定时器与控制周期

当前 PIT 是唯一周期源：

- `PIT_HZ=100`
- 输入时钟 1,193,182 Hz
- divisor 11,931
- 实际约 100.0069 Hz，周期约 9.9993 ms
- IRQ0 向量 `0x20`

因此在没有 one-shot timer / 更高 tick 之前，控制回路的硬上限是 100 Hz，即 10 ms 一个 tick。1 kHz 伺服（1 ms 周期）不能靠现有 tick 直接得到。

## 3. CPU 页实测

在机械臂主画面按 `C` 进入 `AEVUM / CPU TIME ACCOUNTING`，间隔 5 秒抓两次屏：

| 采样 | PIT TICKS | SWITCHES | PREEMPTIONS | control ticks | render ticks | idle ticks |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| A | 286 | 97,684 | 259 | 142 (50%) | 108 (38%) | 36 (12%) |
| B | 700 | 227,574 | 643 | 350 (50%) | 265 (38%) | 87 (12%) |

B 行三任务 ticks 相加为 702，PIT TICKS 为 700；这是同一帧页面采样的 1–2 tick 偏差（任务 ticks 与全局 tick 不是同一时刻原子快照），比例仍按 50/38/12 使用，不把三行相加当成精确总数。

两次采样相差：

- 414 个 PIT tick = 4.1397 秒（按 9.9993 ms/tick）
- SWITCHES +129,890，约 **31,377 次/秒**
- PREEMPTIONS +384，约 **92.8 次/秒**
- 平均每个逻辑切换间隔约 **31.9 微秒**
- control / render / idle 的 tick 增量分别为 +208 / +157 / +51，比例仍是 50% / 38% / 12%

解释边界：

- `SWITCHES` 是调度器“选中另一个任务”的计数，不等价于一次完整的硬件上下文切换成本；它包含 `task_yield()`、`int 0x40` 和 IRQ0 抢占路径。
- 31k 次/秒说明三个常驻任务仍在频繁主动让出，不是每 10 ms 才动一次。`idle` 仍占 12%，没有 `hlt` 停机。
- PIT tick 只决定抢占节拍；任务自己 `yield` 的切换不在这 100 Hz 里。这个差别对实时控制很重要：100 Hz 是“最长不被抢占的粒度”，不是“任务实际切换次数上限”。

## 4. 启动首帧

用 QEMU monitor 在启动后每约 60 ms `screendump` 一次：

- 监视器建立后第一次采样在 69 ms，画面已经是完整机械臂主界面（`PIT 100Hz`、机械臂、`ARM STATUS`、`TASK SCHED`）。
- 25 次采样从 69 ms 到 1,582 ms 都有稳定非空画面。
- 只能说明“首帧不晚于监视器建立后约 69 ms”；QEMU 进程启动前的 BIOS/loader 时间没有被这段采样覆盖，不能当作硬件启动时间。

## 5. 还缺的测量

- 没有 100 轮 fork/wait 的当前墙钟时间；P5.3 的两轮 `100/100` 是历史功能证据，不是本轮速度数据。当前 V 页 D lane 是 4 轮 fork/wait，不是 100 轮。
- 没有测量单次上下文切换的周期数、IRQ 进入/退出延迟、最坏关中断时间。
- 没有真实硬件数据；QEMU TCG 不是实时环境，不能给出机器人关节抖动的可信上限。

## 6. 对后续硬件内核的直接含义

1. **阻塞 wait 已由 P7.5 实现**：`wait` / `thread_join` 不再用 `task_yield_wait()` 轮询，而是在 `cli` 窗口内重查条件后进入 `TASK_BLOCKED`；调度器在任务真实 ZOMBIE 后唤醒同组/父进程组。P7.5 页面给出 `wait_blocks=7`、`join_blocks=1`，对应唤醒计数均非零；等待者不再持续参与 vruntime 竞争。仍未实现超时与 EINTR。
2. **idle 要能停机**：`idle` 占 12% 且频繁 yield；真实硬件上应改成 `hlt`/WFI 或事件驱动，不能让 idle 空转。
3. **控制周期要独立于 100 Hz tick**：1 kHz 伺服需要 1 kHz tick、APIC/HPET one-shot 或独立 PWM/定时器；不能把 100 Hz 当控制周期。
4. **把渲染移出控制路径**：`render` 占 38%，当前和 `control` 共享 PIT tick；硬件上应先保证 control 的截止时间，再让 render 降频或在空闲时间跑。
5. **先测真实切换成本**：31k 次/秒是逻辑计数，不等于 31k 次完整保存/恢复的成本；在目标硬件上要用 TSC/周期计数器测 `task_switch` 前后差值。

## 7. 证据文件

- `D:\Aevum\build\p6-baseline\p60-cpu-a.png`：A 采样
- `D:\Aevum\build\p6-baseline\p60-cpu-b.png`：B 采样
- `D:\Aevum\build\p6-baseline\p60-g-late.png`：G 页重挂后
- `D:\Aevum\build\p6-baseline\p60-vtree.png`：V 页 TREE
- `D:\Aevum\build\p6-baseline\bootf-00.png`：首帧采样（PNG，已从 PPM 转换）
- `D:\Aevum\src\kernel\host\console\robot\kernel.bin`、`disk.img`：本轮 clean 产物

## 8. P7.5 阻塞等待观测

P7.5 clean 构建 `kernel.bin=73,992`，V 页 ADDRESS 在 `EXEC` 行显示 `THR 1 TID 7 blk 7 /1`：

- `wait_blocks=7`：P7.4 与 P7.5 的父进程在子进程 LIVE 时实际进入 BLOCKED 的次数。
- `join_blocks=1`：P7.5 子进程主线程在 worker 仍 LIVE 时实际进入 BLOCKED 的次数。
- `wait_wakes` / `join_wakes` 均非零；`blk OK` 只有在 `WBLK` 与四个计数全部成立时才显示。
- P7.4 的 `grp OK` 在新构建下仍通过，说明把轮询替换为阻塞没有破坏整组回收。该页仍是功能验收，不是延迟测量；单次阻塞/唤醒的周期成本尚未用 TSC 测量。

## 9. P8/P9 动态内核线程观测

P9 clean 构建 `kernel.bin=77,160`，主页面右下显示 `kt 4 s5 a1 OK`，`TASK SCHED P3/31`：

- `kping` 在 control 启动时动态创建，拿到 TID 4 和 1 个 4KB 动态栈槽；它 yield 192 次后退出 5，control 在 `kthread_join` 上实际进入 BLOCKED 一次（`join_blocks=1`），被唤醒一次（`join_wakes=1`）。
- P9 另创建不 join 的 `korph`，它 yield 8 次后退出 6；超过 100 tick grace 后由调度器自动回收。页面 `a1` 是 `auto_reaped=1`，`s5` 是正常 join 读到的退出码。
- `kthread_demo_ok` 要求 status 5、join 前/后 `kernel_stack_used()` 相等、`join_blocks>0`、`reaped=2`、`auto_reaped=1`、`live=0`；页面 `OK` 是这组断言的结果。P8 clean 构建 75,336B，对应旧页面 `kthr 4 s5 OK`。
- 页面右侧常驻 `P3/31` 只统计 control/render/idle；kping/korph 存活期间会占用额外 PID/任务槽，回收后回到 3。该页是生命周期验收，不测 kthread 创建/销毁延迟；192+8 次 yield 在一次启动后 2 秒内完成，未做 TSC 或高频压力测量。
- P7.5 的 `V` 页路径没有新增用户态演示，ring3 代码仍为 4,063B；P9 的改动只发生在 ring0 任务/栈池、调度器回收路径和主页面底栏。`kthread_reap_unjoined()` 每轮调度扫 16 个任务槽，未做单独延迟测量。

## 10. P10 PCB 清零与 exit ring 体积

P10 clean 构建 `kernel.bin=77,544`，`__bss_start=0x92F00`、`__bss_end=0x96EBC`，`AEVUM_KERNEL_END=0x97000`：

- 镜像距 98,304B 上限余量 20,760B；BSS 距 `0x97000` 余量 324B。
- `struct process` 删除每进程 `reaped[2]`（12×32B=384B），新增全局 `exit_ring[4]`（4×48B=192B），BSS 净减 192B；但 `.text/.rodata` 增长 384B，所以 `__bss_start` 前移 384B、`__bss_end` 后移 192B。
- 回收路径多一次 `proc_exit_push()` 的 48B 摘要复制和一次 `proc_zero()`；没有新增锁、没有动态分配。退出 ring 是固定槽，不随进程数增长。
- QEMU 实测 slot 4 从 `PID 8 / ZOMB` 变 `FREE`，`POOL proc 3/12 live 3 zomb 0 dead 0`；`exit 8 st 9` 来自 ring。该页只证明清零和摘要保留，不测 `process_reap()` 的单次周期成本。
