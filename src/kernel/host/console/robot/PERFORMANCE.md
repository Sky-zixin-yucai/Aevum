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

1. **阻塞 wait 已由 P7.5 实现**：`wait` / `thread_join` 不再用 `task_yield_wait()` 轮询，而是在 `cli` 窗口内重查条件后进入 `TASK_BLOCKED`；调度器在任务真实 ZOMBIE 后唤醒同组/父进程组。P7.5 页面给出 `wait_blocks=7`、`join_blocks=1`，对应唤醒计数均非零；等待者不再持续参与 vruntime 竞争。超时由 P11.1 补上，EINTR 由 P11.7 补上。
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

## 11. P11.1 定时阻塞体积与 tick 开销

P11.1 clean 构建 `kernel.bin=78,536`，`__bss_start=0x932E0`、`__bss_end=0x972DC`，`AEVUM_KERNEL_END=0x98000`：

- 镜像距 98,304B 上限余量 29,768B；BSS 距 `0x98000` 余量 3,364B。`AEVUM_KERNEL_END` 从 `0x97000` 修正为 boot 实际读取的 `0x98000`；此前 324B 余量是按 `0x97000` 算的保守口径。
- `struct task_struct` 从 104B 增至 108B（新增 `u32 wake_tick`），16 槽共增加 64B；`tasks=0x6C0`。全局新增 3 个 u32 计数，BSS 总量 `0x3FFC`，比 P10 只多 64B。
- `.text` 从 P10 的 `0x10FC9` 增至 `0x113A0`（+983B），`.rodata=0x1E38`（+36B），`.data=0xE8` 不变。新增代码主要是 `task_block_until_timeout()`、`task_wake_expired()`、`process_wait_common()` 的 timed 分支和 D lane timer 轮。
- PIT 路径每 tick 多一次 16 槽线性扫描；只有 `TASK_BLOCKED && wake_tick` 的任务才做 `(int)(now-wake_tick)>=0`。当前最多 16 个任务，未做 TSC 级测量；`task_pick_next()` 本身也是 16 槽线性扫描，二者同量级。
- `sleep/wait_timeout` 的 deadline 是绝对 tick，不随虚假唤醒重置；同一 tick 内条件满足优先返回 status，不误报 timeout。超时只负责把任务改 READY，不释放子进程、不改 PCB。
- QEMU 计时证据：D lane `sleep(20)` 慢子、`sleep(1)` 快子，父 `wait_timeout(5)` 超时一次，C 页 `SLEEP b 2 w 2 tmo 3`。`tmo 3` = 2 次 sleep 到期 + 1 次 wait_timeout 超时。
- 这不是实时定时器：100Hz PIT 下最小粒度 10ms，`ticks` 不是纳秒级 deadline；没有高精度时钟、没有多等待者队列。EINTR 已由 P11.7 补上。ring3 仍受 4KB code VMA 限制，P11.1 把 129 条 `mov eax, imm` 压成 `push byte imm; pop eax`，code_size 从 4,359B 降到 4,084B，避免 `address_space_create_user()` 拒绝镜像。

## 12. P11.2 优先级读写体积与继承成本

P11.2 clean 构建 `kernel.bin=79,016`，`__bss_start=0x934C0`、`__bss_end=0x974BC`，`AEVUM_KERNEL_END=0x98000`：

- 镜像距 98,304B 上限余量 29,288B；BSS 距 `0x98000` 余量 2,884B。`struct task_struct` 仍为 108B，`tasks=0x6C0` 不变；P11.2 没有新增静态字段或数组。
- `.text` 从 P11.1 的 `0x113A0` 增至 `0x11568`（+456B），`.rodata` 从 `0x1E38` 增至 `0x1E3C`（+4B），`.data=0xE8`、`.bss=0x3FFC` 不变；总镜像 +480B。新增代码主要是 `task_priority_get/set`、两个 syscall case、fork/clone 继承和 V 页优先级显示。
- `task_priority_set()` 的查找是 16 槽线性扫描，写入在 `cli` 窗口内完成；`task_priority_get()` 只扫描读取、不关中断。没有重新计算 `vruntime`，所以一次 set 是 O(16) 查找 + 一次 u32 写，不做 64 位除法；代价与 `task_pick_next()` 同量级，未做 TSC 级测量。
- setpriority 不触发立即抢占：当前任务改高优先级后仍按原时间片继续，下一次 IRQ0 `task_pick_next()` 才按新权重/`vruntime` 选择。不做历史 `vruntime` 重算，避免反复 setpriority 清零累计虚拟时间刷优势。
- 继承路径只多一次 `current_task->priority` 读取：fork 与 clone 原本就要走 `task_create_common()`，没有额外的栈/AS/PCB 分配成本。
- ring3 code 从 P11.1 的 4,084B 增至 4,091B，只增加 7B：P11.2 断言约 +185B，通过把 48 条小立即数加载改为 `push byte/pop`（省 96B）和多组零初始化改为 `xor eax,eax` + `mov [addr],eax`（省约 80B）抵消。4KB code VMA 余量仅 5B，下一次改 ring3 必须先算体积。
- QEMU 证据：V 页 ADDRESS PID 7 显示 `TASK READY P1`、`tmr OK pri OK`；`pri OK` 同时要求默认 1、set 3、非法 4 返回 -1、内核任务拒绝、fork 子继承 3、clone 线程继承 2 全部成立。P5.3 `round 100/100 ... STRESS OK`、`PHYS 6/255`；主页面 `kt 4 s5 a1 OK`；串口无 fault/panic。该页是功能/生命周期验收，不是 setpriority 延迟测量。

## 13. P11.3 等待队列与阻塞 I/O 体积

P11.3 clean 构建 `kernel.bin=81,768`，`__bss_start=0x93F80`、`__bss_end=0x97FDC`，`AEVUM_KERNEL_END=0x98000`：

- 镜像距 98,304B 上限余量 16,536B；BSS 距 `0x98000` 余量仅 36B。`sizeof(struct task_struct)` 从 108B 增至 112B（新增 `waitq` 指针），16 槽 `tasks` 从 1,728B 增至 1,792B，净增 64B；`struct pipe` 每个对象新增两个 1B 等待队列，4 个对象共 8B。BSS 从 `0x3FFC` 增至 `0x405C`（+96B）。
- `.text` 从 P11.2 的 `0x11568` 增至 `0x12029`（+2,753B），`.rodata` 从 `0x1E3C` 增至 `0x1E40`（+4B），`.data=0xE8` 不变；总镜像从 79,016B 增至 81,768B（+2,752B）。新增代码主要是 `task_block_on`/广播唤醒、pipe/console 阻塞接口、fd 阻塞包装、两个 syscall case、image3 与 ring3 lane。
- `wait_queue_wake_all()` 一次广播是 O(TASK_MAX=16) 线性扫描，没有 per-object 等待链表；`task_block_on()` 每次唤醒也重查条件，最坏是一次阻塞对应一次 16 槽唤醒 + 一次条件检查。当前单核 16 槽规模可接受，未做 TSC 级测量。
- `ticks` 是 100Hz PIT 粒度；QEMU TCG 下 guest tick 推进远慢于 wall clock，image3 的 2 tick 超时、20/500 tick 读等待与 100/500 tick 写等待都是功能验收，不是延迟基准。最终 C 页 `IO b 3 w 2 tmo 1` 表示 image3 三次对象阻塞中两次被条件唤醒、一次按 tick 超时返回 -2；`SLEEP b 4 w 4 tmo 6` 是 P11.1 总口径，image3 的超时也计入其中。
- image3 独立镜像 `exec_io.asm` 最终 644B，固定 4KB code VMA，不占 ring3 的 code_size；ring3 本体为 4,061B，比 P11.2 的 4,091B 少 30B，距 4KB 上限余量 35B。原因是新增 lane 的同时复用了零初始化、短立即数和 `xor` 归零压缩。
- QEMU 证据：V 页 ADDRESS PID 7 `io OK`、`REAP1 6 img3 a0 e0`、`CHILDREN live 0 zomb 0`；P5.3 `round 100/100 ... STRESS OK`、`PHYS 6/255`；主页面 `kt 4 s5 a1`。该页证明阻塞/唤醒/超时与 fork 继承路径成立，不是吞吐基准。

## 14. P11.4 idle 停机与 208 扇区窗口

P11.4 clean 构建 `kernel.bin=82,312`，`__bss_start=0x941A0`、`__bss_end=0x981FC`，`AEVUM_KERNEL_END=0x9A000`：

- 镜像距 106,496B 上限余量 24,184B；BSS 距 `0x9A000` 余量 3,588B。P11.3 的 36B BSS 余量已不够容纳 idle 选择/计数代码，因此按仓库既有约定把 boot 第二段从 64 扇区扩到 80，总扇区 192→208，窗口从 98,304B 增至 106,496B；`boot.bin` 仍是 512B，`disk.img` 仍是 1,474,560B。
- `.text` 从 P11.3 的 `0x12029` 增至 `0x121F1`（+456B），`.rodata` 从 `0x1E40` 增至 `0x1E90`（+80B），`.data=0xE8`、`.bss=0x405C` 不变；总镜像 +544B。新增代码主要是 `task_pick_next()` 的双候选扫描、`task_set_idle_task/note_idle_hlt/total`、control/render 的探针分支、idle 的到期唤醒和 C 页 `IDLE hlt` 文案。
- `task_pick_next()` 仍是 O(TASK_MAX=16) 线性扫描，只多了一个 `idle_best` 指针和一次 `t == idle_task` 比较；没有链表、没有第二套 runqueue，也没有额外动态分配。`tasks[]` 仍为 16×112B；BSS 只多 8B（`idle_hlt_total` 与 `idle_task` 各 4B），但被原有对齐吸收，`.bss` 总量未变。
- 正常机械臂负载下 idle 完全不参与选择：C 页 PIT 3,113 tick 时 idle `TICKS 0`、`CPU% 0`、`IDLE hlt 0`，control/render 分摊 57%/43%。这与 P11.3 的 50/38/12 形成直接对比；idle 不再拿固定 12% 份额。
- I 键探针让 control/render 主动 `task_block()` 40 tick，idle 成为唯一 READY；C 页实测 `IDLE hlt 39`、idle `TICKS 39`、`CPU% 1%`，PIT 4,516 tick。40 tick 目标只产生 39 次 hlt，是因为最后一次到期 tick 先唤醒 control/render，idle 未再进入 hlt。
- 探针结束后继续运行到 PIT 8,043 tick，`IDLE hlt` 仍为 39，idle `TICKS` 仍为 39；control/render 的 TICKS 从 2,541/1,936 增至 4,543/3,461。这说明“idle 只在没有非 idle READY 时被选中”不是一次性文案，而是持续的选择规则。
- P5.3 复测 `round 100/100 first 5 -> 5 live 1 state STRESS OK`、`PHYS 6/255`、`proc/AS/KSTK 1/12`；主页面 `kt 4 s5 a1 OK`；串口无 fault/panic。ring3 `code_size` 仍为 4,061B，image3 仍为 644B，没有因 P11.4 改变用户镜像预算。
- 边界：`IDLE hlt` 是 hlt 次数，不是 halt 停留时间；没有 TSC/PMU 级 idle residency 测量，也没有唤醒源分解。QEMU TCG 下 tick 推进慢于 wall clock，39 次 hlt 是功能证据，不是 idle 占比基准。

## 15. P11.5 显式时间片与 PIT 重选频率

P11.5 clean 构建 `kernel.bin=82,664`，`__bss_start=0x94300`、`__bss_end=0x9839C`，`AEVUM_KERNEL_END=0x9A000`：

- 镜像距 106,496B 上限余量 23,832B；BSS 距 `0x9A000` 余量 7,268B。P11.4 的 208 扇区/106,496B 窗口继续够用，本轮没有再次扩 boot。`task_struct` 从 112B 增至 116B，`tasks[]` 从 0x700 增至 0x740；BSS 总量 16,540B（+64B），另加两个 4B 统计全局。
- `.text` 从 P11.4 的 `0x121F1` 增至 `0x12320`（+303B），`.rodata` 从 `0x1E90` 增至 `0x1ED0`（+64B），`.data=0xE8`、`.bss=0x409C`；总镜像 +352B。新增代码主要是 IRQ0 的片记账/提前返回、`task_pick_next()` 的片重置和 C 页 `QUANTUM/EXP`。
- 片长固定 `TASK_SLICE_TICKS=4`；100Hz 下每次连续运行上限 40ms。IRQ0 每 tick 仍做 `task_wake_expired()`、`cpu_ticks/vruntime` 记账，成本与 P11.4 同阶；省掉的是片未尽的 `task_pick_next()` 16 槽线性扫描、切换轨迹和上下文切换。
- 普通协作负载 PIT 1,897 tick：`QUANTUM 4t`、`PREEMPTIONS 0`、`EXP 0`、`SWITCHES 5,725`，control/render 57/43、idle 0%。`SWITCHES` 仍高，是因为 control/render 每轮 `task_yield()`，不是 PIT 抢占；每次 yield 重置整片，所以 `EXP` 为 0 是预期。
- CPU-bound duel：两个 lane 不让出，页面 `PIT PREEMPTIONS 20`，磁带出现青/洋红交替块；停止后 C 页 PIT 4,827 tick，`PREEMPTIONS 100`、`EXP 100`、`SWITCHES 16,040`。100 次片到期全部换了任务，说明 duel 路径每 4 tick 重选一次。
- I 键 idle 探针：PIT 11,676 tick，`IDLE hlt 39`、idle `TICKS 39`、`PREEMPTIONS 100`、`EXP 109`。多出的 9 次是 idle 片到期后没有其它非 idle READY、picker 仍返回 idle；这证明 `EXP` 统计片到期，`PREEMPTIONS` 统计实际换任务，两个口径不能混。
- P5.3 `round 100/100 ... STRESS OK`、`PHYS 6/255`；主页面 `kt 4 s5 a1 OK`；串口无 fault/panic。ring3 `code_size=4,061B`、image3 644B 未变。
- 边界：这是 QEMU TCG 的功能计数，不是调度延迟基准；片固定 4 tick，高优先级只改 `vruntime` 权重，不缩短/延长单次片。没有 per-task 片用完次数、等待时间、队列深度或唤醒延迟统计。

## 16. P11.6 等待队列 FIFO 的存储与唤醒成本

P11.6 clean 构建 `kernel.bin=84,264B`，`__bss_start=0x94940`、`__bss_end=0x98A7C`，`AEVUM_KERNEL_END=0x9A000`：

- 镜像从 P11.5 的 82,664B 增至 84,264B（+1,600B）；BSS 从 16,540B 增至 16,700B（+160B）；镜像距 106,496B 上限余 22,232B，BSS 距 `0x9A000` 余 5,508B。208 扇区 boot 窗口继续够用。
- `sizeof(struct task_struct)` 从 116B 增至 120B（新增 `wait_ticket`），16 槽 `tasks[]` 从 `0x740` 增至 `0x780`（+64B）；`struct wait_queue` 从 1B 增至 8B，9 个队列合计 +63B；新增 `wq_wake_one_total`、`wq_wake_all_total`、`wq_max_depth` 三个 4B 统计全局。三者相加约 +139B，其余 BSS 增量来自对齐和新增 helper 的静态数据。
- 唤醒路径仍是固定的 16 槽线性扫描：`wake_one` 在扫描时多一次票据比较，复杂度与 P11.3 的 `wake_all` 同为 O(16)，没有链表插入/删除、没有动态分配。区别是 `wake_one` 只改一个 BLOCKED 任务，常规 pipe/console 数据到达不再同时放行所有等待者。
- `task_block_on()` 首次入队多一次 `wait_ticket_alloc()` 和深度比较；虚假唤醒后重入队不再分配新票据，只重写 `waitq/wake_tick`，因此不会因为重复唤醒把等待者排到队尾。
- C 页 `WQ blk 9 one 8 all 0 dep 3` 是功能计数，不是延迟基准：`blocks` 复用 P11.3 的 `io_block_total`，`one/all` 是实际唤醒成员次数，`dep` 是任一队列同时 BLOCKED 的最大深度。本轮 image 3 的三段 FIFO 等待把 `dep` 推到 3，`one` 为 8；`all` 为 0 是因为没有走到关闭/EOF 时仍有等待者的广播路径。
- P5.3 复测 `round 100/100 first 5 -> 5 live 1 state STRESS OK`、`PHYS 6/255`；C 页 PIT 11,473 tick 时 `PREEMPTIONS 0 / EXP 0`，再按 `P` 触发 probe 后 PIT 14,948 tick、`PREEMPTIONS 23 / EXP 23`，说明新增字段没有破坏 100 轮 fork/wait 回收，也没有让 PIT 片调度停摆。
- 边界：这是 QEMU TCG 的功能计数；没有测等待延迟、唤醒到运行的时延、队列深度分布或高并发等待的尾部延迟。FIFO 只覆盖已阻塞者，新到者可直接插队，因此不能把它当作严格门票队列的吞吐基准。

## 17. P11.7 EINTR 的存储、扫描与阻塞开销

P11.7 clean 构建 `kernel.bin=85,640B`，`__bss_start=0x94EA0`、`__bss_end=0x98FDC`，`AEVUM_KERNEL_END=0x9A000`：

- 镜像从 P11.6 的 84,264B 增至 85,640B（+1,376B）；`size` 的 text 列从 84,022B 增至 85,369B（+1,347B），`.data=232B` 不变，`.bss=16,700B` 不变。新增代码主要是 `process_signal_take_current()`、三个 block helper 的第三出口、六个调用点的 -3 映射和 C 页一个数字。
- `__bss_start` 随 text 后移 0x560，`__bss_end=0x98FDC`；BSS 距 `0x9A000` 余 `0x1024`（4,132B），镜像距 106,496B 上限余 20,856B。新增的静态数据只有 `eintr_total` 4B，落在 P11.6 的 BSS 对齐余量内，因此 `.bss` 总量不变。
- `sizeof(struct task_struct)=120B`、`sizeof(struct wait_queue)=8B`，P11.7 没有扩 task 结构，也没有新增等待队列字段。
- `process_signal_take_current()` 的最坏扫描是 31 个 bit（1..31），只在条件不满足、deadline 未到时调用；条件满足的快速路径不扫描 pending，因此正常 I/O 命中数据时不会增加这笔开销。命中后清一个 bit 并返回，不排队、不分配、不切任务。
- 阻塞返回路径多一次 `block_rc==2` 分支；没有新增唤醒源。EINTR 与超时/条件满足共用同一次 `task_block_*` 返回，不产生额外的 PIT 扫描或任务切换。
- image 3 代码从 P11.6 的 644B 增至 2,447B（`0x81380..0x81D0F`，+1,803B），仍小于 4KB code VMA；增加的是五段 fork/sleep/pipe/join 用户态测试，不是内核镜像体积。
- C 页实测 `EINTR 7`、`SLEEP b 18 w 14 tmo 16`、`IO b 11 w 8 tmo 1`、`WQ blk 11 one 8 all 0 dep 3`；P5.3 仍 `round 100/100 ... STRESS OK`、`PHYS 6/255`、`proc/AS/KSTK 1/12`，PIT 7222 tick 时 `PREEMPTIONS 0 / EXP 0`、idle 0%。
- 边界：`EINTR` 是功能计数，不是延迟或吞吐基准；没有测“信号置位到阻塞返回”的时延，也没有测多线程争抢同一个进程 pending 位的分布。P11.7 没有引入 `kill()` 注入，不能用它评估完整信号吞吐。

## 18. P11.8 kill、异步投递与 SA_RESTART 的开销

P11.8 clean 构建 `kernel.bin=87,080B`，`__bss_start=0x95440`、`__bss_end=0x9957C`，`AEVUM_KERNEL_END=0x9A000`：

- 镜像从 P11.7 的 85,640B 增至 87,080B（+1,440B）；`size` 的 text 列从 85,369B 增至 86,819B（+1,450B），`.data=232B`、`.bss=16,700B` 不变。新增代码是 `kill`/`sigaction_flags` 两个 syscall、`process_signal_send()`、`process_signal_deliver_async()`、`process_signal_restart_prepare()`、`process_signal_peek_current()` 和 loader 两处投递调用。
- BSS 起点随 text 后移到 `0x95440`，`__bss_end=0x9957C`；BSS 距 `0x9A000` 余 2,692B，镜像距 106,496B 上限余 19,416B。P11.8 没有新增静态计数器或 task 字段，BSS 总量不变；`sizeof(struct task_struct)=120B`、`sizeof(struct wait_queue)=8B` 沿用 P11.7。
- `process_signal_peek_current()`/`take_current()` 最坏扫描 31 个 bit；每次返回用户态最多调用一次 `take`，只在有 pending 时才进入。`process_signal_deliver_async()` 在无 pending 时只有参数/CPL/进程状态检查，正常返回路径增加一次函数调用和一次 31 位扫描。
- `process_signal_restart_prepare()` 只在阻塞 syscall 返回 -3 时调用；先 peek 一次，再读 2 字节用户代码验证 `CD 80`，命中才改帧。正常成功返回不进入该路径，非阻塞 syscall 也不进入。
- `kill` 本身是一次 active 槽查找 + 置 pending + `task_wake_process()` 扫描；没有信号队列、分配或额外任务切换。目标已 READY 时唤醒扫描不改变状态，目标 BLOCKED 时只改 READY，仍由下一次调度决定。
- image 3 代码从 P11.7 的 2,447B 增至 3,208B（+761B），仍小于 4KB code VMA；增加的是四段 kill/SA_RESTART/DFL/CPU-bound 用户态测试，不是内核镜像体积。
- QEMU 实测主页面 `kt 4 s5 a1 OK`、control/render 57/43、idle 0%；C 页 `EINTR 7`、`SLEEP b 18 w 14 tmo 16`、`IO b 11 w 8 tmo 1`、`WQ blk 11 one 8 all 0 dep 3`、`PREEMPTIONS 0 / EXP 0`；V/ADDRESS `REAP1 6 img 3 a0 e0`、`SIG pending 00000000`；P5.3 `round 100/100 ... STRESS OK`、`PHYS 6/255`、`proc/AS/KSTK 1/12`。
- 边界：这是 QEMU TCG 的功能计数，不是信号吞吐或唤醒时延基准；没有测 kill 到 handler 的端到端延迟，也没有测 CPU-bound 场景下 PIT 投递的抖动。`SA_RESTART` 只覆盖 `int 0x80` 返回点，不能代表完整 POSIX 重启语义。


## 19. P12.1 只读 ramdisk 与最小文件 ABI 的体积和读取开销

P12.1 clean 构建 `kernel.bin=89,064B`，`__bss_start=0x95C00`、`__bss_end=0x99D9C`，`AEVUM_KERNEL_END=0x9A000`：

- 镜像从 P11.8 的 87,080B 增至 89,064B（+1,984B）；`size` 的 text 列从 86,819B 增至 88,811B（+1,992B），`.data=232B` 不变，`.bss` 从 16,700B 增至 16,796B（+96B）。新增的是 ramfs 解析/查找/读取、`open` syscall、fd 分支、image4 分支、ramdisk 120B rodata 和 image4 426B 用户代码。
- BSS 起点随 text 后移到 `0x95C00`，`__bss_end=0x99D9C`；BSS 距 `0x9A000` 余 612B，镜像距 106,496B 上限余 17,432B。`struct proc_fd_entry` 从原先直接扩展的 16B 压回 8B，避免 BSS 越 `AEVUM_KERNEL_END`；静态 ramfs 目录只有 4 项。
- `proc_fd_open_ramfs()` 是一次 flags 检查 + 最多 16B copyin + 4 项目录顺序查找；没有分配、没有块 I/O、没有 page cache。`ramfs_read()` 是一次 offset/size 边界检查 + 按 len 的 byte copyout；成功才推进 fd offset，EOF 直接返回 0。`write` 在进入 copy 前拒绝。
- 目录查找是 O(count)，当前 count=2、静态上限 4；文件名比较最多 16B。没有哈希表、inode、目录树或路径解析，因此单次 open/read 的固定成本很小。
- image3 代码从 P11.8 的 3,208B 增至 3,285B（`0x813A0..0x82075`，+77B），仍小于 4KB code VMA；image4 `0x82080..0x8222A` 426B；ramdisk `0x93B54..0x93BCC` 120B。image3 增加的是一次 `spawn(4)` + `wait` 和失败码 30，image4 是独立 426B 镜像。
- QEMU 实测主页面 `kt 4 s5 a1 OK`；V/ADDRESS userD `REAP1 6 img 3 a0 e0`、`CHILDREN live 0`、`SIG pending 00000000`；`THREAD tid 6 join 0 ... ok THRD`；P5.3 `round 100/100 ... STRESS OK`、`PHYS 6/255`、`proc/AS/KSTK 1/12`。image3 exit 0 只有在 image4 返回 0 时成立，因此该证据覆盖 image4 全链路。
- 边界：这是 QEMU TCG 的功能与体积口径，不是文件读取吞吐基准；没有测 open/read 的端到端延迟、缓存命中、并发读或大文件读取。ramdisk 只有 120B，不能代表块设备或 VFS 性能。

## 20. P11.10 RT-1 第一刀：唤醒抢占与最小 RT 类的开销

RT-1 第一刀 clean 构建 `kernel.bin=88,008B`，`__bss_start=0x957E0`、`__bss_end=0x999DC`，`AEVUM_KERNEL_END=0x9A000`：

- 正常 A1 镜像从 RT-0b 的 87,720B 增至 88,008B（+288B）；`size` 的 text 列从 87,455B 增至 87,734B（+279B），`.data=232B` 不变，`.bss` 从 16,796B 增至 16,892B（+96B）。新增的是 `task_struct.rt` 的 4B × 16 槽、`need_resched`/`wake_resched_total` 8B、`task_note_wake`、RT 选择分支和 `task_set_rt/task_wake_resched_total` 两个入口。
- BSS 距 `0x9A000` 余 `0x624`（1,572B）；镜像距 106,496B 上限余 18,488B。正常构建没有 RT 任务，`task_note_wake` 的 RT 分支不会进入；`need_resched` 只在“当前 idle”“目标 RT”或“目标优先级更高”的唤醒上置位。
- IRQ0 正常路径多一次 `need_resched` 判断；没有抢占请求时仍沿用 P11.5 的 `tick_defer` 早退，不做额外 16 槽扫描。有抢占请求时只多一次 `task_pick_next()`，与片到期路径同阶；`task_pick_next()` 多收集一个 `rt_best`，仍是 16 槽线性扫描。
- 所有 READY 唤醒路径多一次 `task_note_wake()` 调用；`task_wake_expired()` 在 PIT 内多一次当前任务/目标比较，不分配、不切栈、不改 `waitq` 票据。`task_wake_resched_total()` 只是返回静态计数器。
- 正常 A1 clean build 三次一致：4 任务 `52.80/26.40/13.60/7.20`，2 任务 `88.80/11.20`；与 P11.9 基线逐位一致。正常内核启动 smoke 无 fault/panic，UI 正常。
- RT 构建（`RT_BG_TASKS=12`）：`kernel.bin=39,844B`、`text=39,598`、`data=228`、`bss=17,016`；`__bss_end=0x8DE38`，距 `0x9A000` 余 4,552B。相对 RT-0b，RT `bss` +96B、镜像 +约 300B。
- 确定性 `-icount` 口径 4/8 后台最大唤醒延迟 19/22us、最大抖动 185/365us、1000 周期零 miss/零 overrun；12 后台 24us/562us。宿主墙钟在 0.5ms 定时器分辨率下 4/8 后台最大延迟 201/405us、抖动 705/820us、零 miss；15.9ms 整机暂停样本仍标 `HOST_INVALID`，不参与门槛。
- 边界：这是单核 100Hz PIT 的功能/延迟观测，不是硬实时保证；没有 APIC one-shot、CBS、RT 队列或 per-task 抢占统计。`need_resched` 是全局单标记，不能表示多个独立抢占请求。1kHz PIT 实测会放大 TCG 宿主抖动，未采用；1ms 伺服必须另做 one-shot 定时器。

## 21. P11.11 RT-2 CBS 预算的体积、账本与宿主口径

RT-2 clean 构建 `kernel.bin=88,712B`，`__bss_start=0x95AA0`、`__bss_end=0x99DFC`，`AEVUM_KERNEL_END=0x9A000`：

- 正常 A1 镜像从 RT-1 的 88,008B 增至 88,712B（+704B）；`size` 的 text 列从 87,734B 增至 88,448B（+714B），`.data=232B` 不变，`.bss` 从 16,892B 增至 17,244B（+352B）。新增的是每任务 6 个预算字段（压缩后 20B × 16 槽）、`budget_overrun`/`throttle` 两个静态计数器、`task_rt_budget_*` 函数和 `task_pick_next()`/`task_schedule_irq()` 分支。
- BSS 距 `0x9A000` 余 `0x204`（516B）；镜像距 106,496B 上限余 17,784B。余量已经接近 RT-1 的 `0x624` 的一半，后续再加 per-task 字段会很快越界；RT-2 已把 `rt_period_ticks` 压到 `u16`、`rt_throttled` 压到 `u8`，没有继续扩 `task_struct`。
- RT 构建（`RT_BG_TASKS=4/8`、`RT_BUDGET=1`、`RT_BUDGET_ONESHOT=0`）：`kernel.bin=40,836B/40,868B`；RT-1 是 39,844B。one-shot 构建 40,964B。RT 镜像仍远低于 106,496B 上限。
- 正常 A1 clean build 三次：4 任务 `52.80/26.40/13.60/7.20`，2 任务 `88.80/11.20`，与 RT-1 基线逐位一致；启动 smoke 无 fault/panic。`task_struct` 的预算字段只增 BSS，不改变正常 A1 的权重/片长/唤醒路径。
- 确定性 `-icount`、tick 级账本、4/8 后台三次：正常 1.8ms 控制 `lat_max=38/42us`、`jitter=207/389us`、`exec=1800us`、`miss=0`、`budget_overrun=0`、`throttle=0`、`switches=2005`。账本开销没有改变 RT-1 的唤醒延迟和切换次数。
- 宿主墙钟 4 后台三次：`lat_max=187/216/285us`、`jitter=594/515/1101us`、`exec=1833/1895/1883us`、`miss=0`、`overrun=0`、`budget_overrun=6/7/7`、`throttle=7/8/7`。宿主 8 后台只有三次重跑各一个有效样本：`lat_max=348/547/228us`、`jitter=8463/3818/1508us`、`budget_overrun=7/9/10`、`miss=0`。宿主停顿把 TSC 墙钟时间计入预算，`budget_overrun` 是误报，不是控制任务真的跑了 2ms。
- 恶意 3ms 控制、tick 级账本：`exec=3000us`、`budget_overrun=1000`、`throttle=1000`、`miss=0`；账本只能在 tick/工作结束发现超预算，**没有 2ms 硬切**。PIT one-shot 确定性 4 后台正常 1.8ms 一次零 miss/零预算超限，`switches=2470`；宿主 one-shot rep1 有效、rep2 `miss=2/late=1` 被 `HOST_INVALID` 拒绝。one-shot 默认关闭。
- 边界：RT-2 当前是“预算记账 + 下一周期 throttle”的第一版，不是硬 CBS；`task_note_wake()` 把一次唤醒当新周期，频繁自唤醒可以重置预算，不能抵御恶意唤醒。2ms 硬截断需要 APIC/HPET one-shot 或独立高精度定时器。宿主 `budget_overrun` 和毫秒级抖动是 TCG 停顿，不应写进实时性承诺。

## 22. P13.1 RT O(1) 就绪队列的体积与路径开销

P13.1 clean 普通构建 `kernel.bin=97,448B`，`__bss_start=0x97CC0`、`__bss_end=0x9C1BC`，`AEVUM_KERNEL_END=0x9D000`：

- 普通构建 text 97,182B、data 232B、bss 17,660B。P13.1 在 `task_struct` 增加 4 个 `u8`，`sizeof(struct task_struct)=148B`；16 槽约 +64B，`rt_head[4]`/`rt_tail[4]`/`rt_mask` 约 +10B。相对 P13.1 前保存的 92,744B 基线，镜像 +4,704B；其中首轮 RT 队列 +544B，第一版 Q 页 +2,368B，本轮双框仪表盘再 +1,792B。
- 上一版 Q 页把 BSS 推到 `0x9C1DC`，越过 `0x9C000` 476B；本轮把 boot 第二段 96→104 扇区、总扇区 224→232，`AEVUM_KERNEL_END=0x9D000`，`kernel.bin` 上限 118,784B；BSS 余 `0xE44`（3,652B），镜像余 21,336B。`boot.asm`、`kernel.ld`、`config/memory.h`、`Makefile` 四处一致。
- 选任务路径：RT READY 存在时，`task_pick_next()` 只做一次 `rt_mask` 判空 + 位图找最高优先级 + 取该优先级队头，成本与 RT 任务数无关；RT 队列为空才进入原来的 16 槽 fair 扫描。fair 扫描多一次 `t->rt` 跳过判断，RT 不再参与 `vruntime` 比较。
- 入队/出队：`rt_enqueue()` 是尾插，`rt_dequeue()` 是按 `rt_prev/rt_next` 摘链，均为固定几步；`task_make_ready()` 把创建、定时唤醒、kthread 唤醒、wait_queue 唤醒、`task_wake` 和 `task_wake_process` 的 READY 写入集中到一处，避免重复入队。`task_priority_set()` 对已入队 RT 先摘链再按新优先级入队。
- IRQ0：RT 不再递减 `slice_left`，`tick_defer` 在无抢占请求时为 1，不再每 4 tick 强制重选；有更高优先级 RT 唤醒时 `need_resched=1`，下一次 IRQ0 立即重选。fair 任务仍按原 4 tick 片和 `vruntime` 记账。
- 队列测试（`RT_BG_TASKS=4 RT_QUEUE_TEST=1`）：`periods=1000 lat_max_us=139 lat_p99_us=70 jitter_max_us=7081 exec_max_us=1866 miss=0 overrun=0 budget_overrun=0 throttle=0 wake_miss=0 sleep_fail=0 late=0 wresched=2011 switches=3024 qerr=0 low_ticks=1010 fifo=1 fifo_a=1 fifo_b=2 pick_max=105855 pick_avg=5437 pick_n=3000 cyc_us=3431 calib=1`。priority 0 控制任务与 priority 1 RT 同跑，控制任务仍零 deadline miss；`qerr=0` 是 4 级头尾、双向链、位图、`rt_queued/rt_prio` 与 READY 状态的结构审计；`fifo=1` 表示两个同优先级 RT 探针按创建顺序 A→B 运行。
- 选任务 TSC 统计在正式周期前重置，排除启动页和校准；本次 `pick_avg=5437` 周期（约 1.5 µs，按 `cyc_us=3431`），`pick_max=105855` 周期（约 31 µs）。QEMU TCG 下单次 max 会受宿主调度抖动影响，因此 max/avg 只作为路径量级证据，不作为硬实时上界。
- 预算越界测试（`RT_BUDGET=1 RT_WORK_US=3000`）：`periods=1000 lat_max_us=152 lat_p99_us=60 jitter_max_us=3988 exec_max_us=3146 miss=0 overrun=1000 budget_overrun=1000 throttle=1000 wake_miss=0 sleep_fail=0 late=0 wresched=2011 switches=3024 qerr=0 low_ticks=1010 fifo=1 fifo_a=1 fifo_b=2 pick_max=158136 pick_avg=5899 pick_n=3000 cyc_us=3430 calib=1`。每个周期 2ms 预算耗尽即摘链，下一周期补满后重新入队；`qerr=0`、`miss=0` 说明 throttle 的摘链/补队没有破坏队列结构，FIFO 探针也没有被预算路径打乱。
- 首轮 RT（`RT_BG_TASKS=4`，无队列测试）`miss=0 overrun=1`；该 `overrun` 是单周期工作超 2ms 预算，不是 deadline miss，`budget_overrun=0`。
- 普通构建没有常驻 RT 任务，`rt_mask` 平时为 0；主页面正常启动，`PIT 100Hz`、`TASK SCHED 3/31`、`mode AUTO`，无 panic。按 `Q` 时两个 priority 1 探针 kthread 短暂入队、记录 A=1/B=2 后退出，页面保留 `FIFO BURST` 卡、`DISPATCH TRACE` 和 `FIFO SLOTS`；返回主画面后机械臂继续更新。
- 边界：这是 QEMU TCG 的功能与结构口径，不是周期级基准；`pick_max/pick_avg` 是 TSC 采样，QEMU 下的 max 会受宿主调度抖动影响。普通主页面只做启动 smoke，没有重跑 P5.3、P11.5、P11.10/11、P12.1 全量回归；其它演示页按用户要求留到以后再修。
- 演示页：按主画面字母键 `Q`（扫描码 `0x10`）进入 `AEVUM // REAL-TIME READY QUEUE`，再按 `Q` 返回。双线框仪表盘含 4 级 `LANE/BIT/READY/HEAD/TAIL/LOAD`、右上两张 `FIFO BURST` 任务卡 `qrtA #1` / `qrtB #2`、中部 `DISPATCH TRACE`、底部位图 / `QUEUED/16` / `ERR` / `SWITCHES` 脉冲条 / `FIFO SLOTS`；普通构建截图见 `p13-q.png`，返回主画面见 `p13-q-return.png`。`rtbench.c` 不再自建独立 VGA 页，只保留串口 `RT_STAT`。

## 23. P13.2 LAPIC one-shot 的代码、页表与验收开销

P13.2 clean 普通构建 `kernel.bin=98,760B`，`text=98,494`、`data=232`、`bss=17,724`：

- `__bss_start=0x981E0`、`__bss_end=0x9C71C`，`AEVUM_KERNEL_END=0x9D000`；BSS 余 `0x8E4`（2,276B），镜像上限 118,784B，余 20,024B；镜像占 193/232 扇区。
- RT bench 构建 `kernel.bin=44,036B`、`text=43,771`、`data=228`、`bss=17,528`。
- 新增 `0x15000` 一张 MMIO 页表，映射 4MB 窗口 `0xFEC00000..0xFF000000`；页表本身 4KB，位于原预留诊断区之外。PDE `0x3FB` 同时复制进用户页目录，避免切换 CR3 后 LAPIC/HPET 访问缺页。
- IDT `0x30` 门和 `lapic_timer_stub` 只增加一个 `SAVE_CTX` 入口；1ms/2ms 状态机是常数级分支，不扫描任务表。
- 开机校准成本：PIT 10ms one-shot + LAPIC 10ms 计数；RT bench 再跑一次 10ms TSC 重校准和一次 1ms TCCR 验证。普通构建只在 boot 做前一次校准，Q 页显示能力，不运行 RT 预算。
- `-icount` 下 RT bench 1000 周期：`servo=2000 hard=1000 armn=1000 gap_us=1000 fgap_us=1000 cut_us=2000 early=0 ifoff=0 qerr=0`；`lat_n=1000`、`wake_miss=0`、`sleep_fail=0`、`switches=4005`。
- `tmr_ticks_ms=61,375`、`one_ms_us=1000`：1ms 倒计数与 TSC 墙钟对齐；`cut_us=2000` 是 arm 到强制 `rt_dequeue()` 的实测墙钟，不是用 10ms tick 反推。
- `miss=1000`、`overrun=1000`、`exec_max_us=10,183` 是墙钟口径：2ms 截断后任务被 throttle 到下一个 10ms tick，函数返回时 TSC 已越过 3ms 工作；它们不能当作截断时延。`pick_avg=725` 周期（约 0.74µs）、`pick_max=3,912` 周期（约 4µs）只作路径量级。
- 默认 QEMU（无 `-icount`）：LAPIC timer 计数正确（`one_ms_us≈1000`），但 IRQ 投递被拖到 TB/主循环边界，`gap_us≈3ms`、`hard=0~192`；这不是 LAPIC 硬件精度，验收命令必须带 `-icount`。
- 边界：单核；没有 IOAPIC 路由、HPET comparator 硬定时、多核 per-CPU LAPIC、RT 组调度、deadline 继承或优先级天花板。PIT one-shot fallback 未做禁用 LAPIC 的回归。全量回归未重跑。

## 24. P13.3 截断预算契约与背景负载矩阵

P13.3 把 P13.2 的“截断一次、看 `cut_us=2000`”扩展为分段统计：`preempt/cut_n/cut_us` 证明截断发生，`post_*` 记录截断到作业返回的墙钟，`after_*` 只记录恢复后实际执行的 TSC。构建统一带 `-DRT_WORK_US=3000 -DRT_BUDGET_ONESHOT=1`，QEMU 统一带 `-icount shift=auto,align=off,sleep=off`。

| bg | hard | cut_n | cut_us | post_avg_us | post_max_us | after_avg_us | after_max_us | lat_max_us | qerr | pick_avg |
|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 1000 | 1000 | 2000 | 9099 | 9100 | 999 | 999 | 7 | 0 | 635 |
| 4 | 1000 | 1000 | 2000 | 9180 | 9181 | 999 | 999 | 7 | 0 | 919 |
| 8 | 1000 | 1000 | 2000 | 9360 | 9362 | 998 | 999 | 32 | 0 | 1193 |

- 三级都保持 `servo=2000 armn=1000 gap_us≈1000 fgap_us=1000 early=0 ifoff=0 wake_miss=0 sleep_fail=0 qerr=0`，`switches=4005`。`cut_us=2000` 是 arm 到强制 `rt_dequeue()` 的实测值，不是按 10ms tick 反推。
- `post_avg_us≈9.1~9.4ms` 是**墙钟**：2ms 截断后任务被 throttle 到下一个 10ms tick，恢复并补跑剩余 1ms，函数在约 11ms 返回，因此截断到返回约 9ms。它包含 8ms 级等待，不是 CPU 执行时间。
- `after_avg_us≈999` 证明补跑确实发生：2ms 截断后恢复再执行约 1ms，3ms 工作跑满；`after_max_us=999` 与 `after_avg` 一致。P13.3 调试中曾出现 `after_max=999 / after_avg=5`，原因是 `rt_work()` 用绝对 TSC 起点，恢复时 `now-start` 已越过 3ms，工作被误判完成；改为只累计 ≤1ms 的循环间隔后消失。
- `miss=1000`、`exec_wall_over=1000`、`exec_max_us≈11ms` 是“3ms 工作 + 2ms 预算 + 10ms deadline”的预期墙钟结果，不是截断失败；截断证据是 `hard=1000 cut_n=1000 cut_us=2000`。
- bg=8 的 `lat_max_us` 从 7µs 升到 32µs、`pick_avg` 从 635 周期升到 1193 周期，是 8 个背景任务把 RT 作业挤出 CPU 后的路径量级上升；`after_avg` 仍为 998µs，补跑没有被背景负载破坏。
- 普通 clean 构建 `kernel.bin=98,952B`、`text=98,677`、`data=232`、`bss=17,820`；`__bss_end=0x9C83C`，BSS 余 1,988B。RT bg=2 `text=44,280`、`data=228`、`bss=17,624`；bg=4/8 text 44,264/44,269。相对 P13.2 的 43,771 增加约 0.5KB。
- 边界：只覆盖单核、`-icount`、bg=2/4/8；不覆盖 IOAPIC、多核 per-CPU LAPIC、RT 组调度、deadline 继承、优先级天花板；默认 QEMU 无 `-icount` 的 `gap_us≈3ms` 不作为验收。全量回归未重跑。

## 25. P13.4 可调度负载与 Linux 对照

P13.3 验证“超预算会截断”；P13.4 把 `work` 降到 `1500µs < budget=2000µs`，看可调度负载能否零 miss。Aevum 仍用 `-icount`、10ms 周期、10ms deadline、LAPIC 1ms one-shot，bg=2/4/8 各 1000 周期。

| bg | hard | cut_n | lat_max_us | lat_p99_us | jitter_max_us | exec_max_us | miss |
|---|---|---|---|---|---|---|---|
| 2 | 0 | 0 | 7 | 0 | 102 | 1500 | 0 |
| 4 | 0 | 0 | 7 | 0 | 184 | 1500 | 0 |
| 8 | 0 | 0 | 8 | 0 | 364 | 1500 | 0 |

`p99=0` 表示低于 1µs 统计粒度；`exec_max=1500` 表示 1.5ms 工作完整跑完，没有被 2ms 预算截断。bg=8 的 `pick_avg` 升到 784 周期，`lat_max` 仍只有 8µs。

`work=2100µs` 边界三级都是 `hard=1000 cut_n=1000 cut_us=2000 after_avg=99 after_max=99`：2ms 截断后只补跑约 0.1ms。默认 10ms deadline 下 `miss=1000`、`exec_max≈10.3~10.5ms` 是 throttle 到下一个 tick 的墙钟结果，不是截断失败；bg=2 的 `post_max/exec_max=13256/15257` 是单次宿主尾样本。

Linux 侧用 i386 v6.18（HZ=100、无 high-res、无 NO_HZ、`PREEMPT_NONE`、SMP=n）新增 `bench_policy=rtlat`：父进程 `SCHED_FIFO 99` 绝对周期唤醒，后台 `SCHED_OTHER`，1500µs 工作，3 次中位数：

| bg | lat_max_us | lat_p99_us | jitter_max_us | exec_max_us | miss |
|---|---|---|---|---|---|
| 2 | 9946 | 9756 | 373 | 1693 | 1000 |
| 4 | 9777 | 9396 | 321 | 1757 | 1000 |
| 8 | 10686 | 9837 | 1205 | 1569 | 1000 |

- 同口径 bg=4：Aevum `p99=0µs / max=7µs / miss=0`，Linux `p99=9396µs / max=9777µs / miss=1000`；唤醒延迟差约 9.4ms（p99）到 1.4×10³ 倍（max）。
- Linux 的 miss 来自 100Hz tick 量化，不是调度器选错任务；1500µs 工作加约 9.4ms 唤醒已经越过 10ms deadline。它不是 PREEMPT_RT，不能当 Linux 硬实时上限。
- CPU 份额仍接近：Aevum A1 4 任务 `52.80/26.40/13.60/7.20` vs Linux CFS `53.34/26.64/13.25/6.74`，最大差约 0.9 个百分点。Aevum 胜在单核周期任务的唤醒确定性，不在通用吞吐。
- 边界：单核、QEMU/TCG、Aevum `-icount`；未覆盖 IOAPIC、多核 per-CPU LAPIC、优先级继承/天花板、PIT fallback、真实硬件；Linux 侧未测 PREEMPT_RT。

## 26. P13.5 RT 互斥锁与优先级继承

P13.5 新增内核内 `rt_mutex`：等待者进 `wait_queue` 前先提升 RT owner，解锁后恢复 `base_priority`。普通构建只多一个锁实现和 3 个任务字段，不改变 fair 调度。

| 项目 | P13.4 | P13.5 | 变化 |
|---|---|---|---|
| `kernel.bin` | 98,952B | 99,048B | +96B |
| `text` | 98,677 | 98,795 | +118B |
| `data` | 232 | 232 | 0 |
| `bss` | 17,820 | 18,044 | +224B |
| `__bss_end` | `0x9C83C` | `0x9C97C` | +320B |
| BSS 余量 | 1,988B | 1,668B | -320B |

`bss` 的 224B 来自 16 个任务槽各 +14B（`base_priority`、`pi_priority`、`pi_boosted`），不是 3B×16 的简单相加；`__bss_end` 还随对齐和新增静态对象后移。普通构建余量仍大于 1.6KB，未触发 boot 扩扇区。

PI 夹具 3 次结果：

| pi | wait_us（r1/r2/r3） | med_ticks | boost | restore | low_p |
|---|---|---|---|---|---|
| 0 | 36,303 / 38,406 / 37,993 | 3 | 0 | 0 | 3 |
| 1 | 7,552 / 7,945 / 7,858 | 0 | 1 | 1 | 3 |

- 无 PI 时中优先级 `pimed` 连续跑满 3 tick，高优先级 `rtctl` 约 36~38ms 才拿到锁。
- 有 PI 时持锁的 `pilow` 继承 RT 0，`pimed` 没有获得 CPU；`rtctl` 约 7.6~7.9ms 返回，等待降到约 1/4.8。
- `wait_us` 含 QEMU 调度和 TSC 读数开销，不是纯临界区 1.5ms；`med_ticks=3→0` 和 `boost/restore=1` 才是 PI 生效的直接证据。
- 两种构建的主循环 `RT_STAT` 均 1000 周期、`miss=0 qerr=0`；PI 夹具两次额外任务阻塞后仍保持队列审计为 0。

边界：单层持锁、仅 RT owner、无嵌套锁/优先级天花板/死锁检测/用户态 mutex ABI/SMP；QEMU/TCG 单核。证据 `tests/kernel/results/2026-11-28/p135-matrix.md`、`p135-pi-r1..r3.log`、`p135-nopi-r1..r3.log`。

证据：`tests/kernel/results/2026-11-28/p134-matrix.md`、`p134-*.log`、`linux-rtlat-bg*-r*.log`。

## 27. P13.6 RT 互斥锁的立即优先级天花板

P13.6 在 P13.5 的等待者 PI 之外增加立即优先级天花板：owner 拿锁当下就提升到静态 `ceiling`，不依赖高优先级任务先进入等待。普通构建不可达，体积与 P13.5 完全一致。

| 项目 | P13.5 | P13.6 | 变化 |
|---|---|---|---|
| `kernel.bin` | 99,048B | 99,048B | 0 |
| `text` | 98,795 | 98,795 | 0 |
| `data` | 232 | 232 | 0 |
| `bss` | 18,044 | 18,044 | 0 |
| `__bss_end` | `0x9C97C` | `0x9C97C` | 0 |
| BSS 余量 | 1,668B | 1,668B | 0 |

`rt_mutex`/RT 夹具只在 `AEVUM_RT_BENCH` 构建中可达；普通镜像被 `--gc-sections` 裁掉，`task_struct` 未增加字段。RT 夹具 `kernel.bin=45,700B`，三种 mode 相同；`disk.img=1,474,560B`。

三模式各 3 次：

| mode | 策略 | wait_us（r1/r2/r3） | lock_p | med_ticks | boost/restore | low_p |
|---|---|---|---|---|---|---|
| 0 | 无继承 | 39,061 / 33,239 / 40,162 | 3 | 3 | 0 / 0 | 3 |
| 1 | 等待者 PI | 7,661 / 9,856 / 11,519 | 3 | 0 | 1 / 1 | 3 |
| 2 | 立即天花板 | 7,078 / 9,083 / 8,374 | 0 | 0 | 1 / 1 | 3 |

- `mode=0`：`pimed` 连续跑满 3 tick，`rtctl` 约 33~40ms 返回。
- `mode=1`：`pilow` 在 high 等锁时继承 RT 0，`pimed` 没有获得 CPU；`rtctl` 约 7.7~11.5ms 返回。
- `mode=2`：`pilow` 拿锁当下就是 RT 0（`lock_p=0`），`pimed` 同样没有获得 CPU；`rtctl` 约 7.1~9.1ms 返回。
- `wait_us` 含 QEMU 调度和 TSC 读数开销，不是纯临界区 1.5ms；`lock_p=3→0` 是天花板与等待者 PI 的直接区别。
- 9 次主循环均 1000 周期、`miss=0 qerr=0`；mode1 r1 的 `exec_max_us=2595 exec_wall_over=1` 出现在 `enforce=0` 构建，是宿主墙钟抖动，不是 budget miss。

边界：单锁、静态 ceiling、仅 RT owner；没有多锁嵌套、ceiling 自动推导、PI 链、死锁检测、用户态 mutex ABI、SMP 或真实硬件。证据 `tests/kernel/results/2026-11-28/p136-matrix.md`、`p136-mode0-r1..r3.log`、`p136-mode1-r1..r3.log`、`p136-mode2-r1..r3.log`。
