# 内核施工说明（归属唯一 / 失败可见）

本目录是 32 位裸机多进程教学内核。`kernel.c` 已拆成模块目录，构建入口仍是
`make clean && make all`，产物仍是 `disk.img` 给桌面「内核实况」读取。

## 一、模块与归属

每个可变字段只有一个写者。其他模块只能通过该模块导出的接口读快照或提交请求。

| 模块 | 文件 | 唯一归属 | 对外接口 |
| --- | --- | --- | --- |
| 内存布局 | `config/memory.h` | 物理地址常量 | 只读宏 |
| 基础类型 | `include/kernel/types.h` | `i32/u32/u16/u8/i64/u64` | 只读类型 |
| 错误入口 | `kernel/error/error.c` | 错误屏、停机 | `aevum_fail()` |
| 任务 | `kernel/task/task.c` | `task_struct` 字段、`tasks[]`、`current_task`、`pid_map`、切换轨迹、`cpu_ticks`/`vruntime`/`fpu_state`；P7.1 增加 `tgid/exit_code` 与线程创建/回收/终止/快照 helper；P7.2 增加 `detached` 标志与调度器延后回收；P7.3 验证主线程 ZOMBIE 后 worker 仍可 join 并继续调度；P7.4 增加 `task_yield_wait()`，只在 wait/join 轮询记 vruntime，普通 yield 不记；P7.5 增加 `task_block_until()` 与 `task_wake_process()`，并在任务真正 ZOMBIE 后触发 `process_notify_task_zombie()`；P8 增加 `kthread/kthread_wait_tid/kernel_entry/kernel_arg` 字段、`task_create_kernel_arg()`、`task_wake_kthread_waiters()`、`task_reap_kthread()`，并让 `task_reap()` 跳过动态 kthread；P9 增加 `zombie_ticks`、`task_reap_unjoined_kthread()`，并在调度器里挂 `kthread_reap_unjoined()` | `task_boot/task_create/task_create_kernel_arg/task_create_thread_user/task_start/task_current/task_exit/task_yield/task_yield_wait/task_block/task_wake/task_pid_used/task_pid_of/task_snapshot_pid/task_preempt_total/task_cpu_total/task_process_live_threads/task_detach_thread/task_reap_thread/task_release_process_threads` |
| 动态 kthread | `kernel/task/kthread/kthread.c` | P8 唯一 owner：`kthread_create` 从 `kernel_stack_alloc()` 取 4KB 动态栈，经 `task_create_kernel_arg()` 建 ring0 任务；`kthread_exit` 置退出码并转 ZOMBIE；`kthread_join` 用 `task_block_until()` 等待目标 ZOMBIE，再回收栈/PID/任务槽；P9 增加 100 tick grace 的 `kthread_reap_unjoined()`，`kthread_stats()` 导出 created/exited/reaped/auto_reaped/failed/join_blocks/join_wakes/live。kthread 没有 PCB，也不进入用户线程组 | `kthread_boot/kthread_create/kthread_exit/kthread_join/kthread_self/kthread_stats/kthread_reap_unjoined` |
| 进程家族 | `kernel/task/family/family.c` | `family[]`、`reaper`、最近一次重挂事件；只写 `ppid` 关系 | `family_boot/set_reaper/add/exit/snapshot/last_event` |
| 用户 PCB | `kernel/proc/process.c` | PCB 槽、PID/PPID、LIVE/ZOMBIE（P10 后回收直接清零，不再产生 DEAD 墓碑）、wait status、fork/exec/reap；P6.0 的 `wait` 先校验直接子进程，并记录最近一次 orphan/reparent 事件；P7.1 以 `process` 为 TGID 资源组，增加 `clone/thread_exit/thread_join/gettid` 和线程组回收；P7.2 增加 `thread_detach`；P7.3 主线程先退只结束当前任务，并移除已无读者的 `process->task` 指针，避免主线程被 join 后悬空；P7.4 `process_snapshot()` 在有任务时改用 `task_info.stack_top`，并让 wait/join 轮询走 `task_yield_wait()`；P7.5 `process_wait` / `thread_join` 改走 `task_block_until()`，新增 `process_notify_task_zombie()` 与 wait/join 阻塞/唤醒计数 | `process_snapshot/process_slot_space/process_slot_fd_info/process_last_orphan/process_clone_thread_current/process_thread_exit_current/process_thread_join_current/process_thread_detach_current/process_exit_snapshot` |
| 地址空间 | `kernel/mm/address_space/address_space.c` | 用户 PD/PT、用户页、CR3 切换与地址空间槽 | `address_space_boot/kernel/create_user/switch/release/cr3/id/set_mode/...` |
| 页错误 | `arch/x86/cpu/fault/page_fault.c` | IDT 14 门、CR2/ERR/EIP 记录、用户 #PF 修复/终止、内核态 panic 页、受控探针 | `page_fault_init/last/count/clear/probe` |
| 同步异常 | `arch/x86/cpu/exception/exception.c` | IDT 0/3/6/13 门、用户 `#DE/#BP/#UD/#GP` 现场、信号映射、默认终止、CPL0 诊断停机 | `exception_init/clear/last/count/vector_count/..._name` |
| TSS | `arch/x86/cpu/tss/tss.c` | `tss.esp0/ss0`、TSS 描述符、`ltr` | `tss_init/tss_set_esp0` |
| 系统调用 | `arch/x86/cpu/syscall/syscall.c` | `int 0x80` 入口、调用计数、最后 CS；分发 `eax=2..23`，新增 `18 spawn`、`19 clone`、`20 thread_exit`、`21 gettid`、`22 thread_join`、`23 thread_detach` | `syscall_init/dispatch/count_total/last_cs` |
| 用户程序 | `user/ring3/ring3.asm` | 被复制进每个用户地址空间的 ring3 代码；同一段代码按 `mode` 进入正常/故障路径；P4.3 增加第四轮 pipe/fork/exec/read/wait；P4.4 增加 mode 4 console 父子共享 fd 0/1；P7.1 在 D lane 增加 `clone` worker、共享计数器、`thread_exit` 与 `thread_join`；P7.2 增加自 detach 的第二个 worker 与 `DETD` 验收；P7.3 增加 join/detach 非法参数矩阵和“主线程先退、worker 继续”子进程；P7.4 增加“主线程不 join、两个 worker 继续、最后线程触发整组回收”子进程；P7.5 增加“父 wait 阻塞、子主线程 join 阻塞、worker 退出后双层唤醒”子进程 | `user_program_start/end` |
| 用户 exec 镜像 | `user/exec/exec.asm` | P4.2 的第二个 flat 镜像，固定链接在 `0x400000`；从新栈校验 `argv/envp`，写 PID/`EXEC`/`ARGV`/`ENVP` 后 `exit(10)`，失败码 20..25 | `user_exec_start/end` |
| 用户 pipe 镜像 | `user/exec-pipe/exec_pipe.asm` | P4.3 image 2，固定链接在 `0x400000`；从 `argv` 取 fd，向继承的写端写 4 字节 `PIPE` 后 `exit(12)`，失败码 26..31 | `user_pipe_exec_start/end` |
| 每进程 fd 表 | `kernel/proc/fd/fd.c` | 8 槽 fd 表、fork 复制、close_all、pipe/read/write/close 与用户指针校验；fd 0/1 是 console 读/写，kind 区分 console 与 pipe；P5.2 增加逐 fd `used/kind` 快照 | `proc_fd_table_*/proc_fd_pipe/read/write/close/console_open/proc_fd_snapshot` |
| 匿名 pipe | `kernel/ipc/pipe/pipe.c` | 4 个对象、64 字节非阻塞环形缓冲、`refs/readers/writers`、最后引用释放后回池 | `pipe_create/acquire/release/read/write/live_count` |
| console 设备 | `drivers/console/console.c` | 16 字节 US ASCII 输入队列、8×64 输出行缓存、COM1 镜像、scan/push/drop 统计 | `console_input_push/read/write/reset/...` |
| console 演示 | `app/console/demo/demo.c` | mode 4 用户进程；父 fork 后关闭自身 0/1，子进程继续读 fd0/写 fd1 | `console_demo_start/stop/alive/...` |
| console 渲染 | `app/console/render/render.c` | 独立 X 全屏页，显示 close 边界、输入队列、输出行和 UART 字节 | `console_render_frame` |
| 用户演示 | `app/user/demo/demo.c` | A/B/C/D/F 的创建、停止、地址空间和 PID；D 的子进程由 fork 产生；D lane 在 wait 前 yield 262144 次，留出可观察 ZOMBIE 窗口；P7.1 增加 `clone` 后的共享计数器、join 返回值和 `THRD` 验收字；P7.2 增加自 detach worker 的 `DETD` 验收；P7.3 增加 `EDGE` 与 `MTHR` 验收；P7.4 增加 `GRP4` 与 `MGRP` 验收；P7.5 增加 `BLK5` / `WBLK` 验收与 `user_demo_block_ok()` | `user_demo_start/stop/alive/error/pid/space/thread_tid/thread_join_ret/thread_shared/thread_ok/thread_det_*/thread_edge_ok/main_first_ok/thread_group_ok` |
| V 页导航 | `app/user/page/page.c` | 页面模式、选中 PCB 槽、10 行窗口；扫描码解释的唯一 owner | `user_page_reset/key/mode/selected_slot/first_slot/name` |
| V 页渲染 | `app/user/render/render.c` + `draw.c` | 公共标题/页签/slot 状态/底栏/页面分派；`draw.c` 统一十进制/十六进制、进程/VMA/fd 状态名与颜色 | `user_render_frame` |
| V 页子页 | `app/user/render/slots.c` / `tree.c` / `address.c` / `fd.c` | 每页只读 PCB/AS/fd/信号快照并画自己的行布局；P6.0 的 TREE 页只读 `process_last_orphan()`；P7.1 的 ADDRESS/SLOTS 页显示 `THR/TID` 与 `THREAD tid/join/shared/THRD`；P7.2 的 ADDRESS 页增加 `det OK`，用于验证 detached worker 自 detach 与 join 拒绝；P7.3 增加 `main OK` 与 `arg OK`；P7.4 增加第 20 行 `grp OK`；P7.5 增加第 15 行 `blk wait_blocks/join_blocks` 与第 21 行 `blk OK` | `ur_render_*` |
| 端口 I/O | `include/arch/x86/io.h` | `in/out` 指令封装 | `port_inb/port_outb`（内联） |
| 中断/定时 | `arch/x86/cpu/interrupt/interrupt.c` | IDT、8259A PIC、PIT 100Hz、IRQ0 EOI | `interrupt_init/interrupt_set_user_gate/interrupt_eoi` |
| VGA | `drivers/video/vga/text/vga.c` | 影子显存 `0x20000`、`0xB8000` 刷新 | `vga_*` |
| 串口 | `drivers/serial/uart/uart.c` | COM1、`0x30000` 快照、差量帧缓冲 | `uart_*` |
| 键盘 | `drivers/input/keyboard/keyboard.c` | 无状态；只读扫描码 | `keyboard_poll()` |
| 定点 | `lib/fixed/fixed.c` + `include/lib/fixed.h` | Q16.16 运算 | `fx_mul/fx_div/isqrt64/fx_sqrt` |
| 三角 | `lib/math/trig.c` | `sin_tab`、`fx_sin_deg` | `fx_sin/fx_cos/fx_deg/fx_rad` |
| 格式化 | `lib/format/format.c` | 十进制/十六进制/定点转字符串 | `fmt_dec/fmt_hex/fmt_hex_width/fmt_fx` |
| 运动学 | `app/arm/kinematics/kinematics.c` | 纯函数，无状态 | `arm_fk/arm_jacobian/arm_solve` |
| 控制 | `app/arm/control/control.c` | `q/dq/points/target/...` 与 `arm_status` 快照 | `arm_control_init/step/toggle_nullspace/status` |
| 渲染 | `app/arm/render/render.c` | 轨迹环形缓冲 `trace[]`；只读 `arm_status` | `arm_render_init/frame/trace_push` |
| 应用装配 | `app/arm/app/app.c` | `p_control/p_render/p_idle`；创建三个常驻任务；键盘分发（含 `V` / `K`） | `arm_app_init/start` |
| FPU 上下文实验 | `app/fpu/demo/demo.c` | `active/stop/done[]/lanes[]`；两个任务各自维护 XMM0 标签与计数 | `fpu_demo_start/stop/live/read` |
| FPU 实验渲染 | `app/fpu/render/render.c` | 全屏 VGA 布局；只读 `struct fpu_lane` 快照 | `fpu_render_frame` |
| 家族树演示 | `app/tree/demo/demo.c` | `famP/famA/famB` 的创建、停止、done 标志；父进程退出由 `task_exit()` 触发 | `tree_demo_start/stop/alive/error` |
| 家族树渲染 | `app/tree/render/render.c` | 全屏树形页；只读 `family_entry` 与 `task_info` 快照 | `tree_render_frame` |
| 组合根 | `kernel/main/main.c` | 只做初始化编排 | `kernel_main()` |

约束：

- `task_struct` 对 `task.c` 以外不透明；外部不能直接写 `state`。
- 渲染只读 `struct arm_status` 快照，不直接读控制器的私有变量。
- VGA 只通过 `vga.c` 写；串口只通过 `uart.c` 写。
- `kernel_main` 不持有业务状态，只按依赖顺序装配并处理装配失败。

## 二、结构化错误

错误身份是 `(owner, field)` 字符串对，不再分配 `0xRRFFTT` 数字码。

- `owner`：出错模块
- `field`：失败字段名，失败函数直接返回该字符串
- `type`：分类标签（`missing` / `conflict` / `dependency` / `external` / `state`）
- `detail`：具体原因
- `at`：调用点

失败函数成功返回 `0`，失败返回字段名字面量，例如：

| 返回值 | 场景 |
| --- | --- |
| `"out"` | 出参为空 |
| `"entry"` | 入口为空 |
| `"stack"` | 栈顶越界或未 16 字节对齐 |
| `"slot"` | 任务槽满 |
| `"pid_hint.range"` | PID 提示越界 |
| `"pid_hint.used"` | PID 提示已被占用 |
| `"pid_pool"` | PID 位图 1..31 已耗尽 |
| `"state"` | 状态非法或当前任务不是 RUNNING |
| `"schedule"` | 调度器无法选出下一个 READY 任务 |
| `"pid"` | 唤醒时找不到 BLOCKED 任务 |
| `"space/proc/pid"` | 用户任务创建参数不全 |
| `"frame/space/proc/pid"` | fork 参数不全 |
| `"process_create_user"` | 用户演示创建进程失败 |

`aevum_fail()` 是唯一停机入口，错误屏必须带五要素：`owner`、`field`、
`type`、`detail`、`at`。`aevum_fail` 只打印这些结构化字段，不再打印十六进制错误码。

## 三、进程状态

`task_struct.state` 只有 `task.c` 能写。当前已经跑通的边：

| 起始 | 触发 | 目标 | 实现 |
| --- | --- | --- | --- |
| `UNUSED` | 创建任务 | `READY` | `task_create()` 末尾发布 |
| `READY` | 调度器选中 | `RUNNING` | `task_schedule_irq()` |
| `RUNNING` | 主动让出 / PIT 时间片到期 | `READY` | `task_yield()` / IRQ0 的 `task_schedule_irq()` |
| `RUNNING` | 主动阻塞 | `BLOCKED` | `task_block()` |
| `BLOCKED` | 别的任务唤醒 | `READY` | `task_wake(pid)` |
| `RUNNING` | 任务退出 | `ZOMBIE` | `task_exit()` |
| `ZOMBIE` | 调度器下一次进入 | `UNUSED` | `task_reap()` 释放 PID |

`RUNNING` 的三种离开方式现在统一走 `task_set_current_state(state)`：
`TASK_READY` 等价 `task_yield()`，`TASK_BLOCKED` 等价 `task_block()`，`TASK_ZOMBIE` 等价
`task_exit()`。设置 `TASK_RUNNING` 是无副作用 no-op；非 `RUNNING` 的当前任务传其他状态返回
`"state"`。这样 `state` 仍只有 `task.c` 一个写者，不会出现应用直接改状态后不切调度的情况。

`task_pick_next()` 只选择 `READY`，因此 `BLOCKED` 和 `ZOMBIE` 不会被调度。
PIT 通道 0 以 100Hz 触发 IRQ0，`task_schedule_irq()` 在 IRQ0 和 `int 0x40` 两条入口上做同一套调度；
`px` 是 IRQ0 实际造成的抢占切换数，`sw` 是全部切换数（含主动 `task_yield`）。

### 3.1 优先级加权公平选择

- `priority` 取值 `0..3`，权重 `weight = priority + 1`，即 `1..4`。
- 每个 PIT tick 给正在运行的任务 `vruntime += 64 / weight`。
- `task_pick_next()` 在 READY 任务中选择 `vruntime` 最小者；同值从 `prev` 之后的槽位轮转，避免固定顺序。
- 新建任务从 `current_task->vruntime` 起跑，不从 0 值插队。
- 高优先级任务获得更大 CPU 份额，低优先级任务仍会前进，不会被严格最高优先饿死。

三个常驻任务的权重是 `control=4`、`render=3`、`idle=1`，理论 CPU 份额约 `50% / 37.5% / 12.5%`。

### 3.2 每任务 CPU 时间

- `struct task_struct` 增加 `cpu_ticks` 和 `vruntime`；`struct task_info` 暴露 `cpu_ticks`。
- 每次 IRQ0 且 `prev->state == RUNNING` 时：`prev->cpu_ticks++`、`prev->vruntime += 64 / weight`。
- `task_cpu_total()` 汇总仍存活任务的 `cpu_ticks`，所以 CPU 页各行百分比相加为 100%。
- 调度器选中当前任务自己时，必须把状态恢复为 `RUNNING`；否则一次 yield 会把任务永久留在 READY，PIT 不再给它计费，`vruntime` 不再增长，最终独占 CPU。
- `C` 键（扫描码 `0x2E`）打开全屏 `AEVUM / CPU TIME ACCOUNTING`：PID、TASK、PRI、WT、STATE、CPU%、TICKS、SHARE，底部 PIT TICKS、SWITCHES、PREEMPTIONS。

当前 `BLOCKED` 仍没有超时和 sleep，`task_wake()` 必须由另一个任务显式调用。

### 3.3 当前任务状态写入口

`task_set_current_state(state)` 不是“改一个字段就返回”，而是立刻触发调度：

- `TASK_RUNNING`：当前任务已是 RUNNING，直接返回 0，不产生切换。
- `TASK_READY`：以 `SCHED_YIELD` 进入 `int 0x40`，调度器把 `prev` 置 READY 并选择下一个任务。
- `TASK_BLOCKED`：以 `SCHED_BLOCK` 进入调度；若无其他 READY 任务，调度器恢复 `RUNNING` 并返回错误，和原 `task_block()` 一致。
- `TASK_ZOMBIE`：以 `SCHED_EXIT` 进入调度，`prev` 置 ZOMBIE，`task_reap()` 释放 PID，调用永不返回。

`task_yield()`、`task_block()`、`task_exit()` 都改为调用这个入口，B 键演示因此实际走的就是
`RUNNING -> BLOCKED -> READY -> RUNNING` 的同一套状态写入路径。

### 3.4 FPU / SSE 上下文

- 每个任务栈底部预留 512 字节 FXSAVE 区，16 字节对齐；`task->fpu_state` 指向它，`stack_base` 后移 512 字节，实际可用栈 3.5KB。
- `task_boot()` 先 `fninit`，把初始 FPU/SSE 状态分别存进 `fpu_template` 和 `boot_fpu_state`；新任务从模板复制，不从零初始化。
- 调度器只在 `next != prev` 时执行 `fxsave(prev->fpu_state)`、`fxrstor(next->fpu_state)`；同任务不切换则不动 FPU。
- `loader.asm` 打开 `CR4.OSFXSR/OSXMMEXCPT`，清 `CR0.EM/TS` 并置 `MP`，否则 `fxsave/fxrstor` 不可用。
- 这一节只讲 CPU 状态。地址空间、CR3 和 ring3 在 3.7；`ctx.esp` 只是帧指针，FPU/SSE 是第二块 CPU 状态。

### 3.5 FPU 上下文实验

`F` 键创建 `fpuA` / `fpuB` 两个 CPU 密集型任务，循环里不调用 `task_yield()`；两个任务只在 `XMM0` 里维护自己的标签和计数。`app/fpu/demo/demo.c` 是 `lanes[]`、`active/stop/done[]` 的唯一写者，`app/fpu/render/render.c` 只读快照并画 `CTX OK/LOST`。如果切换漏掉 `fxsave/fxrstor`，标签会串到另一条 lane，屏幕直接判 `CTX LOST`；`F` 再按一次停止并回到机械臂。

全局 `CFLAGS` 使用 `-Os -mno-sse`，只有 `app/fpu/demo` 追加 `-msse2`。这不是为了省事：如果编译器在普通任务代码里生成 SSE 暂存，它会在调度器执行 `fxsave` 前改写 `XMM0`，把任务的标签保存成 0，屏幕上就会看到 `TAG 0 / CTX LOST`。

### 3.6 进程家族树

`kernel/task/family/family.c` 单独维护 `pid -> ppid` 表，`task.c` 只负责在创建和退出时通知它：

- `task_create()` 在发布 READY 前调用 `family_add(pid, current_pid)`，新任务是当前任务的直接子进程
- 内核任务 `task_exit()` 进入 ZOMBIE 后调用 `family_exit(pid)`，只遍历 `ppid == pid` 的直接子进程；带 `proc` 的用户任务不走这条路径
- 重挂目标是 `family_set_reaper(idle_pid)` 设置的 `idle`；若 reaper 是退出任务的子孙，回落到 `init(0)`，避免父子成环
- 这版只描述内核任务树：内核任务没有 exit-status 等待，`task_reap()` 仍立即回收 PID；用户进程的 `fork/exit/wait/reap` 由 `kernel/proc/process.c` 单独提供，不复用 `family` 表

`G` 键展示真实树：`famP` 创建 `famA` / `famB` 后退出，两个子进程被移到 `idle` 下面，右侧事件面板记录父 PID、两个子 PID 和新父亲。`app/tree/render/render.c` 只读 `family_snapshot()` 和 `task_snapshot_pid()`，不直接改关系。

### 3.7 进程模型、地址空间与 ring3

P1 建立用户 PCB 与 `fork/exit/wait/reap` 闭环；P2.1 把地址空间从“固定 4 页 + 固定 AS 槽”推进到“物理页池 + VMA + brk + guard + copyin/copyout”；P2.2 把 `fork` 从 eager copy 改成 COW，并给物理页加引用计数；P3.1 接通用户同步异常的默认终止、信号 pending/SIGCHLD 和 wait status 编码；P3.2 增加用户 handler、signal frame 与 `sigreturn`；P4.1 增加内嵌 flat 镜像 `exec`；P4.2 增加 `argv/envp` 解析、字符串迁移和 SysV 初始栈；P4.3 增加每进程 fd 表、匿名 pipe 与 `pipe/read/write/close` 系统调用，并验证 exec 后 fd 表保留；P4.4 把 fd 0/1 接到 console 字符设备，验证键盘输入、VGA/COM1 输出、fork 继承与 close 边界。职责仍是三分：

- `kernel/proc/process.c`：用户 PCB、PID/PPID、`PROC_LIVE/ZOMBIE`（P10 后 `PROC_DEAD` 仅保留兼容枚举，不再由回收路径产生）、退出码、信号字段、`fork/wait/reap/cleanup` 的唯一写者。它只持有 `address_space *`、`task_struct *` 和内嵌 `proc_fd_table`，不直接改 PTE，也不切上下文、不直接读写 pipe 缓冲。
- `kernel/task/task.c`：任务槽、内核栈、调度和 FPU 的唯一 owner；`task_struct.proc` 只是对 PCB 的反向引用，不反向拥有 PID。
- `kernel/mm/address_space/address_space.c`：PD/PT、VMA、用户页、物理页池、brk、CR3 和缺页修复的唯一 owner。`address_space_create_user()` 从位图池分配 PD/PT/code/data/stack/control 六页；`address_space_clone()` 只分配新 PD/PT，把可写用户页共享为 COW；`address_space_release()` 遍历 PT 逐页归还；`address_space_set_brk()` 增删 heap 页；`copyin/copyout` 通过 VMA + PTE 翻译，不再让内核直接解引用用户指针。
- `arch/x86/cpu/exception/exception.c`：IDT 0/3/6/13 的唯一 owner，记录用户同步异常现场并按信号默认动作终止当前进程；`arch/x86/cpu/fault/page_fault.c` 继续独占 IDT 14，两者不互相安装对方的门。
- `kernel/task/family/family.c` 只保留内核任务树：带 `proc` 的用户任务不再调用 `family_add()`，调度器也不对用户 ZOMBIE 调用 `family_exit()`。`G` 页展示内核任务树，`V` 页展示用户进程树，两者是两套关系，不再互相写。

地址空间和用户进程：

- 内核空间固定使用 `0x10000` 页目录，`0..4MB` 恒等映射保持 supervisor-only；VGA BIOS 影子 ROM `0xC0000..0xFFFFF` 不放进页池。
- 用户物理页池 `0x100000..0x1FEFFF` 共 255 页，位图分配，每页一个引用计数；避开内核 `0x80000..0x96000`、VGA/BIOS 和 `0x200000` 起的任务栈。
- 每个用户空间有独立 PD/PT；用户 VA 固定为 code `0x400000`（RX）、data `0x401000`（RW）、guard `0x402000`（VMA 无 PTE）、stack `0x403000`（RW，栈顶 `0x404000`）、control `0x404000`（R）、heap `0x405000..0x410000`（RW）。`fork` 只分配新 PD/PT，可写用户页共享为 COW，control 页因内核要写停止字也走 COW。
- 6 个 VMA 是 `code/data/guard/stack/control/heap`；`address_space_user_range_ok()` 按 VMA 权限 + PTE present/user/RW 双重校验，`handle_user_fault()` 只对 heap VMA 内、brk 以下的缺页零填并 `iret` 重试，guard/RO/非法地址一律返回 0。
- 用户内核栈来自 `AEVUM_KSTACK_BASE=0x200000` 起的 12 个 4KB 槽位图池：槽 0 顶 `0x201000`，槽 11 顶 `0x20C000`；创建/fork 时 `kernel_stack_alloc()` 取槽，失败路径和 reap 时 `kernel_stack_free()` 归还并整槽清零。每槽底部 512 字节仍是 FXSAVE 区，实际可用 3.5KB。
- `task_create_user_with_pid()` 构造 14 槽中断帧：`cs=0x1B`、`ss=0x23`、`eflags=0x202`，并使用进程先保留的 PID。`task_fork_user()` 复制父进程的 ring3 中断帧、通用寄存器、用户栈和 FPU；子进程帧 `eax=0`，父进程系统调用返回时拿到子 PID。
- `V` 演示创建 A/B/C/D/F 五个 lane：A/B 递增各自的 `0x401000` 计数器，并做 `brk(0x406000)`、`poke(0x405800,"HELP")`、`peek`、越界 `brk(0x500000)`、`poke(guard)`；C 用 `mode=1` 数到 5000 后写 `0xB8000`；D 用 `mode=2` 做四轮 fork/wait：第一轮子进程写 `CHLD` 后 `exit(7)`，第二轮子进程先 `sigaction(SIGFPE,handler,0)` 再 `1/0` 触发 `#DE`，handler 写 `0x401054` 计数，`sigreturn` 回到除零后的指令，最后 `in al,0x60` 触发 `#GP -> SIGSEGV(11)`；第三轮子进程 `eax=13, ebx=1, ecx=argv, edx=envp` 调 `exec(1, argv, envp)`，参数为 `alpha/beta/NULL` 与 `MODE=p42/LEVEL=2/NULL`；新镜像从新栈校验四串后写 `ARGV/ENVP` 并 `exit(10)`；第四轮父 `eax=14 pipe()` 得到 fd 2 读 / fd 3 写，fork 后父关闭 fd 3，子关闭继承的 fd 2 后 `exec(2, {"pipe","writer","FD=3"})`，镜像仍持有 fd 3 并向其写 4 字节 `PIPE` 后 `exit(12)`，父 `wait` 得到 12 后从 fd 2 读回并校验 `0x45505049`，最后关闭 fd 2；F 用 `mode=3` 写 guard `0x402000`。
- P3.2 实测 D 的 PID 7、E1/E2 都复用 PID 8、PPID 7；第一轮 `fork 8 / wait 8 / status 7`，第二轮 `fork2 8 / wait 8 / st 00000009`，V 页显示 `HAND OK`，信号面板 `#DE eip 0040021B sig 8 SIGFPE hdl 1 chld 2 pend 0`。C/F 由 `process_terminate_current(SIGSEGV)` 终止，PCB 行显示 `SIG11`；E2 正常 `EXIT 9`，不再显示 `SIG8`。A/B 继续计数。
- 上述 P3.2 记录保留当时 `exit(9)` 的构建结果；当前 sig 轮加入 EFLAGS 白名单验收，改为 `in al,0x60` -> `#GP -> SIGSEGV(11)`，父 wait 得到 `0x8000000B`。
- A/B/C/D/F 由 render 内核任务通过 `process_create_user()` 创建，E1/E2/E3/E4 是用户父进程 `fork` 创建的进程。P3.2 证明 VMA、brk、guard、copyin/copyout、物理页回收、COW、同步异常默认终止、用户 handler、signal frame 和 `sigreturn`；P4.2 补 `exec` 的 `argv/envp` 解析、字符串迁移和 SysV 初始栈；P4.3 补每进程 fd 表、匿名 pipe 及 exec 后 fd 保留；P4.4 补 console fd 0/1、fork 继承与 close 边界。仍不证明 ELF、文件系统/路径 `open`、console 会话/前台焦点、`kill`、完整 POSIX 信号队列/阻塞管理。
- P4.2 `exec` 的唯一入口是 `process_exec_current(frame,image_id,argv,envp)`，系统调用为 `eax=13`、`ebx=image_id`、`ecx=argv`、`edx=envp`；P4.2 只接受 `image_id==1`，P4.3 增加 `image_id==2`。它先校验 CPL3/LIVE/有 AS，在旧 AS 仍有效时调用 `proc_exec_parse()` 解析两个向量：最多 `PROC_EXEC_MAX_VEC=4` 项，每串最多 `PROC_EXEC_STRING_MAX=32` 字节（31 字符 + NUL），数组必须有 NULL 结尾，超限或不可读直接失败；解析结果只落在内核 `proc_exec_args`，不保留用户指针。随后 `address_space_create_user(image_start,size)` 分配新 AS，`proc_exec_build_stack()` 把字符串和指针数组复制到新用户栈：`strings_base=(0x404000-string_bytes)&~3`、`envp_addr=strings_base-env_words`、`argv_addr=envp_addr-argv_words`、`esp=argv_addr-4`，布局为 `argc, argv[], NULL, envp[], NULL, 字符串区`；2 项参数 / 2 项环境时 `esp=0x403FC8`。exec 只换 AS/镜像，不调用 `proc_fd_table_close_all()`，也不重置 fd 表。
- 切换阶段仍在 `cli` 下 `address_space_switch(new_space)` 写 CR3，`task_set_space()` 更新任务持有的 AS 指针和 `self->space`，重置 `sigactions`/`sig_mask`/`pending`/`sig_handled`，把 iret 帧改成 `eax..ebp=0`、`eip=0x400000`、`cs=0x1B`、`eflags=0x202`、`user_esp=new_esp`、`ss=0x23`，最后释放旧 AS。解析、AS 分配或栈构造任一步失败都释放新 AS、保留旧 AS/frame，返回 -1。
- `struct process` 的 `image_id`：初始 ring3 镜像为 0，`exec` 镜像为 1；`fork` 继承，P10 起 `process_reap()` 先写入全局 exit ring 再清零 PCB，不再保留墓碑。`exec_argc/exec_envc` 也在 exec 成功时写入 PCB，`fork` 继承，`process_snapshot()` 暴露给 V 页。`process_image_id(pid)` 与 `process_exec_counts(pid)` 先查活 PCB，再回退到同 PID 的最新 ring 记录。
- `user/exec/exec.asm` 与 `user/ring3/ring3.asm` 都固定链接到 `0x400000`，由内核 `Makefile` 分别汇编成 `build/user/exec.o` 和 ring3 目标；运行时不读盘，镜像内容直接进内核。exec 镜像先 `getpid` 写 `0x401000`、写 `EXEC` 到 `0x401004`、记录 `esp` 到 `0x401008`，然后从 `[esp]` 读 `argc`，逐字节校验 `argv[0]=alpha`、`argv[1]=beta`、`argv[2]=NULL`、`envp[0]=MODE=p42`、`envp[1]=LEVEL=2`、`envp[2]=NULL`；全部匹配才把 `ARGV/ENVP` 写到 `0x401018/0x40101C`，yield 300 次后 `exit(10)`。失败分支写 `BAD` 并以唯一失败码退出：20 argc、21 argv0、22 argv1、23 envc、24 env0、25 env1，便于 QEMU 直接定位。
- `user/exec-pipe/exec_pipe.asm` 是 P4.3 的 image 2，同样固定链接 `0x400000`。它从 `argv={"pipe","writer","FD=3"}` 解析 fd，校验 `argc=3`、`argv[0]=pipe`、`argv[1]=writer`、`argv[2]=FD=3`；随后向该 fd 写 4 字节 `PIPE`，成功 `exit(12)`。失败分支以唯一码退出：26 argc、27 argv0、28 argv1、29 argv2、30 fd、31 write；数据页 `0x401000` 起记录 pid、`PIPE/BAD`、fd、write 返回值、payload、esp、argc 与 `"PIPE"` 数据。
- **fd 表**：`kernel/proc/fd/fd.c` 是唯一 owner，PCB 内嵌 `struct proc_fd_table fds`；`PROC_FD_MAX=8`，fd 0 为 console 读、fd 1 为 console 写，`PROC_FD_FIRST=2`，pipe 从 2 起分配。每槽只有 `endpoint` 和 `kind`（`CLOSED/PIPE_READ/PIPE_WRITE/CONSOLE_READ/CONSOLE_WRITE`）；`process_fds()` 只暴露表指针，read/write/close 的用户指针校验和 `copyin/copyout` 都在 fd 模块内完成。
- **fd 生命周期**：`fork` 用 `proc_fd_table_copy()` 逐槽复制，只对 pipe kind `pipe_acquire()`，console kind 直接继承；`exec` 不触碰 fd 表；`exit`、`process_terminate_current()` 和 `process_reap()` 都 `proc_fd_table_close_all()`，只对 pipe kind `pipe_release()`；`proc_zero()` 也先关闭再清 PCB，防止槽复用漏引用。新用户进程创建成功后 `proc_fd_console_open()` 安装 fd0 读、fd1 写。
- **pipe 对象**：`kernel/ipc/pipe/pipe.c` 固定 `PIPE_MAX=4` 个对象、每对象 `PIPE_CAPACITY=64` 字节环形缓冲；维护 `refs/readers/writers`，`pipe_acquire()` 按端点加计数，`pipe_release()` 减引用，最后一个引用释放后立即清空并回池。读写都是非阻塞：空读且仍有写端返回 `-1`，写端全关返回 `0`，满写返回 `-1`，成功返回字节数。
- **P4.3 系统调用**：`eax=14 pipe(ebx=user u32[2])` 写读/写 fd；`15 read(ebx=fd,ecx=user_buf,edx=count)`；`16 write(...)`；`17 close(ebx=fd)`。失败统一 `-1`；read 最多 `PIPE_CAPACITY` 字节，write 超过容量或指针不可读直接失败。
- **D 第四轮**：父 `pipe()` 得到 fd 2/3；`fork` 后父关闭 fd 3，子关闭继承的 fd 2 后 `exec(2)`；exec 镜像仍持有 fd 3，写 `PIPE` 并 `exit(12)`。父 `wait` 得到 12 后从 fd 2 读 4 字节，校验 `0x45505049`，关闭 fd 2；V 页写 `P4.3 pipe 8 wait 8 st 0000000C fd 2>3 r4 PIPE OK`，两端释放后 `pipe obj 0`。
- **P4.4 console 设备**：`drivers/console/console.c` 是唯一 owner。`CONSOLE_INPUT_MAX=16`，输入环形队列保存 US ASCII；`console_input_push()` 接收 render 已轮询的 scancode set 1，支持 `0x2A/0x36` 与 `0xAA/0xB6` 的 Shift，release 码和未知键忽略。`read(0)` 非阻塞，空队列返回 `-1`，有数据返回 1..len。`CONSOLE_LINES=8`、`CONSOLE_LINE_MAX=64`、`CONSOLE_WRITE_MAX=64`；`write(1)` 追加行缓存并逐字节 `uart_write()` 到 COM1，X 页只读行缓存渲染。
- **P4.4 X 页演示**：`app/console/demo` 创建 mode 4 用户进程；ring3 先 `fork`，父进程关闭自己的 fd0/fd1 并验证 `read(0)=-1`、`write(1)=-1`，然后设置 `CON!` 并保持存活；子进程继承 fd0/fd1，循环非阻塞读键，读到后写 `CON:<key>` 和 `FD01 OK`，再 `exit(14)`。父进程不调用 `wait`，避免 X 页额外占用轮询；P4.3 的 D 第四轮已经单独验证 `wait`。页面以 `out 14` 与 `child EXIT 14` 判定子进程完成，以 `close0 0 close1 0 r0 -1 w1 -1` 判定 close 边界，以输出行和 UART 字节判定双写通路。
- **P4.4 页面**：X 页是独立全屏模式，在 `proc_render` 的命令键分支之前处理；`X=0x2D` 进入/退出，进入后其他按键进入 fd0。页面显示 pid/child/out、CLOSE OK、close/read/write 结果、输入总数/最后字节/队列/drop、输出总数/UART 字节和 8 行 console 输出；按 `X` 停止父/子后清场回机械臂。
- **PID 复用下的历史证据**：P4.2 与 P4.3 的 exec 子进程都复用 PID 8，单靠当前 PCB 无法稳定区分两行。P4.2 当时在 PCB 里加 `PROC_REAP_HISTORY=2` 的 `reaped[2]`，由 `process_wait()` 在回收前记录 `pid/image_id/argc/envc`；P10 删除这份每进程历史，改由 `process_reap()` 统一写入全局 `exit_ring[4]`。`process_reaped_exec(parent_pid,slot,...)` 按 `ppid` 返回该父进程最近两次退出：slot 1 最新、slot 0 次新，不再固定对应 P4.2/P4.3。
- `AEVUM_AS_MAX` 从 6 提到 8：exec 期间旧 AS、新 AS 和 fork 子 AS 必须同时存在，7 个槽会在第三轮 fork+exec 时耗尽。
- `exception_info` 增加 `handled` 快照：`exception_dispatch()` 在 `process_signal_deliver()` 成功后记录当时的 handler 计数，V 页不再按 PID 扫描进程槽。PID 8 被 exec 子进程复用时，P3.2 的 `sig_handled` 墓碑可能已被 `proc_alloc_slot()` 清掉，按 PID 重查会显示 `hdl 0`。
- `int 0x80` 是 DPL3 中断门：`eax=2` yield、`3` exit(ebx=code)、`4` wait(`ebx=pid,ecx=user status`)、`5` getpid、`6` fork、`7` getppid、`8` brk(ebx=new_brk)、`9` peek(ebx=user ptr)、`10` poke(ebx=user ptr,ecx=value)、`11` sigaction(`ebx=sig,ecx=handler,edx=mask`)、`12` sigreturn、`13` exec(`ebx=image_id,ecx=argv,edx=envp`)、`14` pipe(`ebx=user u32[2]`)、`15` read(`ebx=fd,ecx=user buf,edx=count`)、`16` write、`17` close(`ebx=fd`)。`wait` 仍用 `task_yield()` 轮询；`copyout` 写 status，不直接解引用用户指针；fd 调用的用户指针校验在 fd 模块内完成。
- 同步异常入口在 `loader.asm`：`#DE/#BP/#UD` 没有 CPU error code，stub 先补 `err=0` 再压 vector；`#GP` 由 CPU 压 error code，stub 只压 vector；四条路径共用 `exception_common`，帧布局与 `struct irq_frame` 对齐。
- `exception_dispatch()` 的 CPL3 路径记录 `pid/cs/vector/sig/eip/err/cr2/comm` 后先调用 `process_signal_deliver()`：有用户 handler 时写 signal frame 并返回同一 `irq_frame`；没有 handler 或 frame 写入失败才调用 `process_terminate_current(sig)`。`#DE -> SIGFPE`、`#BP -> SIGTRAP`、`#UD -> SIGILL`、`#GP -> SIGSEGV`。CPL0 的非 #PF 同步异常没有 fixup，进入 `AEVUM / KERNEL TRAP` 后停机。
- 信号状态：`pending` 用 `1u << (sig-1)`；子进程正常退出或被信号终止都会给 LIVE 父进程置 SIGCHLD。`process_wait()` 成功回收 ZOMBIE 后清父进程 SIGCHLD pending；P10 起 `process_reap()` 先把 `term_sig/exit_code/comm` 等摘要写进 exit ring，再 `proc_zero()`，V 页改读 ring 的最新记录。
- 每进程 `sigactions[32]` 保存 `handler/mask/flags`，`sigaction` 只接受 1..31 和用户 VA；`fork` 复制 handler 表与 mask，不复制 pending。`sig_mask` 只随 signal frame 保存/恢复，不做阻塞。
- wait status：正常退出 `exit(7)` 仍返回 `7`；默认信号终止返回 `0x80000000 | sig`，`#DE` 为 `0x80000008`。`PROC_WAIT_EXIT_CODE()` 取低 8 位，`PROC_WAIT_SIGNAL()` 取低 7 位信号号。
- PID 复用：`process_fork()` 从 exit ring 取该父进程最近退出的直接子 PID，再 `task_pid_reserve_hint()`；不再依赖 DEAD 墓碑。V 页按 `comm[4]=='C'/'F'` 判 FAULT/GUARD，不按旧 lane PID 猜。
- 调度记账：`task_schedule_irq()` 对 `int 0x40` 的 `SCHED_YIELD` 也按 `WEIGHT_QUANTUM / weight` 记 vruntime，防止 `wait` 轮询的父进程一直保持最低 vruntime 而饿死其他 lane。
- `V` 页直接读 `process_snapshot()`：列为 `PID PPID ST EXIT CR3 AS PGS/C BRK COMM`；`EXIT` 列对 `term_sig` 显示 `SIG11/SIG8`，正常退出显示 `EXIT 9/10/12`。P10 起回收后 PCB 立即清零，exec 子进程的 `image_id=1/2` 不再保留在墓碑；`SLOT` 页底部 `exit <pid> st <status>` 改读全局 exit ring 最新记录。另有 VMA 摘要、heap/guard 结果、COW 统计、`PHYS PAGES used/total`、`pipe obj`、`int 0x80 traps`、fork/wait 摘要、#PF 面板、`#DE` 面板（含 `hdl` handler 计数）、`P4 exec` 面板（`arg/env` 计数）、`P4.3 pipe` 面板（fd 2>3、r4、`PIPE OK`）和 `SIGCHLD n pend` 行。
- `page_fault_dispatch()` 的用户路径先 `address_space_handle_user_fault()`；修好 heap 缺页就返回原帧重试，否则 `process_terminate_current(SIGSEGV)` 再 `task_exit()`，所以 C/F 的 `EXIT` 是 `SIG11`。内核态受控探针条件不变：`probe_armed`、`CR2=0x00400000`、CPL0、`ERR.P=0`、`CR3=内核 PD`。
- 调度器 `task_reap()` 只自动回收没有 `proc` 的内核任务，并调用 `process_reap_orphans()` 回收没有活父进程的用户 ZOMBIE；`process_reap()` 统一释放任务槽、地址空间、PID 和内核栈，P10 起再把退出摘要写入全局 exit ring 并清零 PCB。

### 3.8 V 页分页与观测面（P5.2）

P5.2 把 `V` 页从单体渲染推进为“UI 状态 + 页面绘制”分离，并没有新增第二套进程/页表状态：

- `app/user/page/page.c` 只持有 `page_mode`、`page_slot`、`page_first`；`USER_PAGE_COUNT=4`、`USER_PAGE_ROWS=10`。`user_page_key()` 是 `V` 页扫描码的唯一解释者，`app/arm/app/app.c` 只在 `user_live` 时转发。`Tab` 循环，数字 `1..4` 直达，`Up/Down` 移槽，`PgUp/PgDn` 滚 10 槽，`Home/End` 跳首尾；`0xE0` 前缀在 page 层吞掉。
- 页面分派在 `render.c`，子页在 `slots.c/tree.c/address.c/fd.c`；`draw.c` 唯一负责 `put_dec/put_hex`、进程状态名/颜色、VMA kind 名和 fd kind 名。`render.c` 不再复制格式化逻辑。
- `SLOTS` 用 10 行窗口覆盖 12 个 PCB 槽，列 `SLOT/PID/PP/ST/TK/EX/ASID/PGS/COMM`；选中槽详情列 KSTACK TOP、CR3/ASID/PGS/C/BRK/VMA、image/argc/envc/switches/ticks、信号、children 的 live/zomb/dead 与 WAITER、池统计。
- `TREE` 按 PPID 递归画用户进程树，根 `R0`、子节点 `+depth`；每行 `CH` 统计直接子进程数、`W` 统计 ZOMBIE 子进程数；visited 防环，未访问节点补画为 root。
- `ADDRESS` 列 6 条 VMA 的 START/END/FLAGS 与真实 PTE R/W 探针，另有 COW/PHYS、信号、exec/reap 历史、COW last、children/WAITER；底部保留 P3/P4 验收字。
- `FD/SIG` 列 8 个 fd 槽的 kind/open、fd 打开数、pipe 端点数、信号、最近 `#DE`、最近 `#PF`、pipe 对象数、children/WAITER；`proc_fd_snapshot()` 只交 `used/kind`，不暴露 pipe 指针。
- `process_slot_space()` 与 `process_slot_fd_info()` 让页面按 PCB 槽取 AS/fd；FREE 槽返回空。`process_slot_used()` 只数 LIVE/ZOMBIE，`POOL proc` 另行显式统计 `live+zomb+dead`；P10 起回收槽直接变 FREE，`dead` 固定为 0。
- ZOMBIE 观察窗口由 D lane 在父 `wait` 前执行 `0x4010D0=262144` 次 yield 提供；子进程退出后保持 ZOMBIE 数秒，父随后仍执行原 wait/COW 验收。QEMU 实测 `PID 8 slot 4 PP 7 ST ZOMB EX 7`、`PID 7 CH 1 W 1`、窗口 `3-12/12`；wait 完成后 `TEST wait/cow/hand/exec/pipe` 全部 `OK`。

边界：32 位、平坦段、单核、255 页固定物理池的最小实现。没有 ELF 加载、文件系统、路径 `open` 或 console 会话/前台焦点，也没有多核；`exec` 只接受内嵌 flat 镜像 1/2，`argv/envp` 只支持最多 4 项、每串最多 31 字符，不是通用装载器。fd 表只有 8 槽，fd 0/1 是单个全局 console 的读/写端点，pipe 池只有 4 个对象、每对象 64 字节且非阻塞，不具备阻塞队列、命名 pipe 或文件语义。信号已有每进程 handler 表、signal frame、`sigreturn` 和 SIGCHLD pending，但没有 `kill()`、信号排队或真正阻塞的 mask；`#DE` 只解码 `F6/F7`，其他同步异常直接 `ret` 会重试 faulting 指令。`fork` 是 COW，但 `wait` 是 yield 轮询；`family` 和 `process` 是两套树；非探针内核 trap 仍终止性停机。

### 3.9 100 轮 fork/wait 压测（P5.3）

P5.3 在 P5.1 的 `Y` 池页外壳上增加 `S` 模式，不新增页面、不新增第二套计数：

- `include/app/pool/demo.h` 只增加 `POOL_STRESS_ROUNDS=100`、`pool_demo_start(int stress)` 与 `pool_demo_is_stress()`；`pool_demo_start` 仍唯一持有 `poolA/stressA` 的创建、模式写入和首次 yield，`stress ? 6u : 5u` 决定 ring3 压测还是 P5.1 池压力。
- `user/ring3/ring3.asm` mode 6：父进程 100 轮 `eax=6 fork`，子进程直接 `eax=3, ebx=7 exit(7)`；父进程以本轮返回子 PID 调 `eax=4 wait`，校验返回 PID 等于子 PID、`0x401108` status 等于 7，首轮子 PID 写 `0x401100`，后续 99 轮必须与首轮相同（PID 复用）。全部通过后写 `0x4010C4="POOL"` 并保持存活，按 `S` 停止时 `exit(18)`。
- 压测复用 P5.1 数据布局，不复制结构体：`0x4010C0` 是已完成轮次，`0x4010C4` 是完成魔数，`0x4010C8` 是只执行一次标记，`0x401100/0x401104/0x401108` 是首 PID/末 PID/wait status。页面仍由 `app/pool/render/render.c` 唯一绘制；`pool_demo_child_count()` 在 stress 下返回 0..100 轮次，并跳过 8 项 PID 缓存循环，避免越界。
- 页面复用现有池统计，不额外调用 `process_live_count/process_slot_used/address_space_slot_used/kernel_stack_used`：`round 100/100 first 5 -> 5 live 1 state STRESS OK`，`PHYS 6/255 proc 1/12 AS 1/12 KSTK 1/12`。`state STRESS OK` 只在魔数写入后出现；任何一轮 wait 返回 PID/status 不符或 PID 未复用，轮次停在 `<100`、不写魔数，页面显示 `RUN`。
- 构建预算：`Makefile` 的 C 目标增加 `-ffunction-sections -fdata-sections`，链接增加 `--gc-sections`，按 `ENTRY(start)` 保留可达函数/数据。P9 clean 构建为 77,160 字节，上限 98,304 字节。

### 3.10 P6.0 细节收口

P6.0 不新增页面、不合并两套树，只修已有进程/家族树的语义和显示边界：

- `process_wait(pid, status)`：`pid > 0` 时先用 `proc_is_direct_child()` 检查 `LIVE|ZOMBIE` 直接子进程；不是子进程或已经不存在立即返回 -1。`pid <= 0` 等待任意直接子进程。扫描条件从 `pid >= 0` 改为 `pid > 0`，因此 `waitpid(0)` 不会再误等 PID 0。
- `proc_orphan_children()`：父进程退出或异常终止时，同时把 `LIVE` 和 `ZOMBIE` 的直接子进程 `ppid` 改成 0；`process_reap_orphans()` 仍负责回收无活父的 ZOMBIE。每次有子进程被改挂，写 `last_orphan`：`valid/seq/from/to/child_count/children[4]`；`process_last_orphan()` 只读导出。
- V 页 TREE：`tree.c` 的节点上限从 row 21 收到 row 19，row 20/21 画 `ORPHAN from N to 0 children N` 与 `kids pid,pid +N more`；row 22 保留 TREE 统计，row 23/24 保留公共底栏。
- G 页：`draw_event()` 的子进程行从 3 行扩到 4 行（row 8..11），`reaper` 下移到 row 12；`child_count > AEVUM_FAMILY_EVENT_CHILDREN` 时在 row 13 显示 `+N more`，总数仍显示原 `child_count`。
- 当前速度基线另见 `PERFORMANCE.md`：100 Hz PIT，4.14 秒窗口 `SWITCHES +129,890`、`PREEMPTIONS +384`，control/render/idle 50/38/12；P9 clean `kernel.bin=77,160`，距 98,304 上限余 21,144B。

### 3.11 P7.4 主线程不 join 的整组回收

P7.4 不改 clone/join 语义，只把“主线程退出后没人 join，最后由最后线程触发整组回收”补成可验收路径：

- D lane 在 P7.3 成功后 fork 一个子进程。子进程主线程 `gettid`，再 `clone` worker A 与 worker B；主线程只写 `0x4011D4=1` 退出标记，然后 `thread_exit(0)`。组内仍有 3 个活线程，所以当前任务只转 ZOMBIE，进程保持 LIVE。
- worker A 等标记后直接 `thread_exit(5)`；worker B 等标记后 `thread_join(A_tid, &status)`，校验 join 返回 0、status 5，写 `GRP4`，再作为最后活线程 `thread_exit(7)`。此时 `task_process_live_threads(self)<=1`，进入 `process_exit_current()`。
- 父进程 `wait` 得到子 PID、status 7，写 `MGRP`；`process_reap()` 调 `task_release_process_threads()`，把主 ZOMBIE、A、B 三个任务槽/内核栈一次释放，再释放 AS。
- `process_wait()` 与 `process_thread_join_current()` 的轮询从 `task_yield()` 改为 `task_yield_wait()`：普通 yield 不记 vruntime，只有等待轮询先记 `WEIGHT_QUANTUM / weight` 再切走。修复前 `wait` 不记账，D 以最低 vruntime 反复抢占，spawn 子进程被饿死；全量 yield 记账又让 D 的 26 万次 yield 把 vruntime 抬高，D 反而长期得不到 CPU。P7.4 当时按“只记等待轮询”折中，QEMU 已验证 spawn 与 P7.4 都能推进；P7.5 已改为真正阻塞，不再依赖轮询记账维持公平。
- `process_snapshot()` 在 `has_task` 分支改用 `task_info.stack_top`，避免主线程被 join 后展示过期 `process->stack_top`；P7.4 主线程未被 join，但快照口径保持一致。
- V 页 ADDRESS 第 20 行追加 `grp` 标签和值，读取 `0x4011F8` 的 `MGRP`；列位置 74/78，80 列边界内收尾。

### 3.12 P7.5 阻塞 wait/join 与 lost wake 防护

P7.5 把 P7.4 的等待轮询换成真正的 `BLOCKED -> WAKE`，同时保留“条件重查”：

- `task_block_until(ready, ctx)`：`pushf` 保存 flags 后 `cli`；调用 `ready(ctx)`。返回非 0 表示条件已满足，恢复 flags 返回 0；否则写 `current_task->state=TASK_BLOCKED`、`sched_reason=SCHED_BLOCK`、`sched_failed=0`，再 `int $0x40`。从 `cli` 到调度门之间没有 PIT，也没有其他任务可运行，因此“重查”和“进入阻塞”是不可分割窗口。
- `process_wait()` 的 `proc_wait_ready()`：有可回收 ZOMBIE 子进程、指定 PID 已不是直接子进程、或没有子进程时返回 1；`thread_join` 的 `proc_join_ready()`：目标不存在、detached、或 ZOMBIE 时返回 1。外层 `for(;;)` 被唤醒后继续走完整扫描、错误路径或回收路径。
- 调度器挂接：`task_schedule_irq()` 在 `prev->state == TASK_ZOMBIE`、`fpu_save(prev->fpu_state)` 之后、`task_reap()` 之前调 `process_notify_task_zombie(prev->proc)`。此时目标任务不能再运行，`proc` 指针尚未被清；等待者重查一定能看到 ZOMBIE，避免在 `process_thread_exit_current()` 提前唤醒造成的 lost wake。
- `process_notify_task_zombie()` 先 `task_wake_process(proc)` 唤醒同组 join 等待者；若 `proc->state == PROC_ZOMBIE` 且 `ppid>0`，再找父进程并唤醒父进程组，覆盖 `wait`。
- 全局 `wait_block_total / wait_wake_total / join_block_total / join_wake_total` 只统计 wait/join 的实际阻塞与唤醒，不含 idle；`process_block_stats()` 导出。
- ring3 P7.5 轮次：父 fork 后立即 `wait`（子仍 LIVE，阻塞）；子主线程 `clone` worker 后立即 `thread_join`（worker 仍 LIVE，阻塞）；worker 等 `0x401208=1` 后 yield 1024 次并 `thread_exit(4)`；子主线程 join 得到 4，写 `BLK5`，`thread_exit(9)`；父被唤醒得到子 PID/status 9，写 `WBLK`。
- V 页第 15 行 `blk wait_blocks/join_blocks`，第 21 行 `blk OK`；`blk OK` 要求 `WBLK == 0x57424C4B` 且四个阻塞/唤醒计数全部非零。

### 3.13 P8 动态内核线程

P8 把“内核任务只能启动时静态创建”这条限制拆掉：

- `task_create_kernel_arg(priority,name,entry,arg,stack_top,&task)` 与 `task_create()` 共用 `task_create_common()`，但额外写入 `kernel_entry/kernel_arg/kthread=1`。初始中断帧仍进入 `task_kernel_arg_entry()`，由 trampoline 读当前任务的 fn/arg，调用后再 `task_exit()`；没有改初始帧的 GPR 约定。
- `kthread_create()` 先在 `kernel_stack_alloc()` 取栈，再建任务；任务创建失败时立即 `kernel_stack_free()` 回滚。只允许 `task_process_current()==NULL` 的 ring0 任务调用。
- `kthread_exit(code)` 只允许 `task_current_is_kthread()` 为真，写 `exit_code` 后 `task_exit()`；静态 control/render/idle 误调用会 `cli; hlt`，不会变成无人回收的 ZOMBIE。
- `task_reap()` 对 `state==TASK_ZOMBIE && proc==NULL` 的槽增加 `!kthread` 条件，动态 kthread 优先留给 joiner；无人 join 时由 P9 的 grace reaper 兜底。
- `kthread_join()` 先 `task_set_kthread_wait(tid)`，再进 `task_block_until()`。`kthread_join_ready()` 在 `cli` 窗口内重查：目标不存在/已不是 kthread/已 ZOMBIE 都返回 1，外层再区分错误或回收。调度器在 `prev->kthread` 变 ZOMBIE 后调用 `task_wake_kthread_waiters(prev->pid)`，只唤醒等待该 TID 的任务；唤醒时保留 `kthread_wait_tid`，等 `kthread_join()` 真正回收后再清。
- `task_reap_kthread(tid)` 校验目标仍是 kthread ZOMBIE，依次 `kernel_stack_free(stack_top)`、`pid_free(pid)`、`task_zero()`；`task_zero` 同时清 kthread 标记、等待 TID、`zombie_ticks`、fn/arg。
- control 启动时创建优先级 0 的 `kping`，它 yield 192 次后 `kthread_exit(5)`；control 优先级 3，先进入 join 的 BLOCKED 再让目标运行。P8 页面证据为 `kthr 4 s5 OK`，后由 P9 扩展为 `kt 4 s5 a1 OK`。

### 3.14 P9 未 join kthread 的 grace 兜底

P9 补的是 P8 留下的真实泄漏：`kthread_exit()` 后没有人 join 的 ZOMBIE 会永久占住任务槽、TID 和 4KB 动态栈。

- `task_struct` 增加 `u32 zombie_ticks`，`task_zero()` 清零；`task_schedule_irq()` 在 `SCHED_EXIT` 把任务转 ZOMBIE 时写 `tick_total`。
- `kthread.c` 定义 `KTHREAD_ZOMBIE_GRACE_TICKS=100u`。100Hz PIT 下是 1 秒：`kthread_create()` 返回 TID 后，创建者仍有调用 `kthread_join()` 的窗口。
- 调度器在 `task_reap_detached()` 之后、`process_reap_orphans()` 之前调用 `kthread_reap_unjoined()`；它把 `task_tick_total()` 和 grace 传给 `task_reap_unjoined_kthread()`。
- `task_reap_unjoined_kthread(now, grace)` 只扫描 `TASK_ZOMBIE && kthread`，跳过 `current_task` 和 `now - zombie_ticks < grace` 的目标；再扫全部非 UNUSED 任务，只要任一 `kthread_wait_tid == target->pid` 就跳过，保证有人正在 join 时绝不抢收。
- 无人等待且过 grace 时，依次 `kernel_stack_free(stack_top)`、`pid_free(pid)`、`task_zero()`，返回回收数量。`kthread_reap_unjoined()` 把数量同时累加到 `reaped_total` 和 `auto_reaped_total`，`live = created - reaped` 仍守恒。
- `task_wake_kthread_waiters()` 只把 joiner 置 READY，不在此处清 `kthread_wait_tid`；`kthread_join()` 在 `task_reap_kthread()` 成功后才清，失败路径也清。否则“已唤醒但还没取走退出码”的 joiner 可能被兜底 reaper 抢先，join 返回 -1。
- 策略边界：grace 过后才来 join 的调用者得到 -1。这不是 join 超时，而是“没有人等在 grace 内”的自动回收；正常 join 仍先拿退出码并回收，不走兜底。

P9 的内核自测仍由 control 启动时执行一次：

- 保留 `kping` 的 join 用例：TID 4、192 次 yield、退出 5；control 在 `task_block_until()` 上实际阻塞。
- 新增 `korph`：只 yield 8 次，然后 `kthread_exit(6)`，故意不 join。
- control 等 140 tick（大于 100 tick grace），再检查 `status==5`、`kernel_stack_used()` 回到创建前、`join_blocks>0`、`reaped==2`、`auto_reaped==1`、`live==0`。
- 主页面右下显示 `kt 4 s5 a1 OK`：`s5` 证明正常 join 没被抢，`a1` 证明兜底路径真的回收了 1 个 kthread，`OK` 是上述整组断言的结果。


## 四、内存与构建预算

| 地址 | 用途 |
| --- | --- |
| `0x80000` | 内核镜像起点 |
| `0x80000..0x90000` | boot 第一段 LBA 读取：128 扇区（64KB）窗口 |
| `0x90000..0x98000` | boot 第二段 LBA 读取：64 扇区（32KB）窗口；当前 `kernel.bin` 77,160 字节，结束于 `0x92D68` |
| `0x92D80..0x96DFC` | 当前构建的 BSS；`__bss_start=0x92D80`、`__bss_end=0x96DFC`，低于 `AEVUM_KERNEL_END=0x97000` |
| `0x9000..0x9FFF` | `render` 任务栈（栈顶 `0xA000`） |
| `0xA000..0xAFFF` | `idle` 任务栈（栈顶 `0xB000`） |
| `0xB000..0xBFFF` | 内核主线程栈（栈顶 `0xC000`） |
| `0xC000..0xCFFF` | `probe` 任务栈（栈顶 `0xD000`） |
| `0xD000..0xDFFF` | `spinA` 任务栈（栈顶 `0xE000`） |
| `0xE000..0xEFFF` | `spinB` 任务栈（栈顶 `0xF000`） |
| `0x1FF000..0x1FFFFF` | `control` 任务栈（栈顶 `0x200000`） |
| `0x200000..0x2BFFFF` | 用户进程内核栈池：12 个 4KB 槽，栈顶 `0x201000..0x20C000`；槽 0..4 示例 A..E，槽 5..11 由位图按需分配 |
| `0x10000 / 0x11000` | 内核页目录 / 页表（0–4MB 恒等映射） |
| `0x13000..0x13FFF` | `K` 探针恢复用的 PDE 1 页表 |
| `0x14000..0x14FFF` | `K` 探针恢复用的诊断页（承接 `0x00400000` 写） |
| `0x12000..0x127FF` | 256 项 IDT（0x20 IRQ0、0x40 软件调度门、0x80 ring3 系统调用门） |
| `0x8FC80..0x8FCBF` | 内核 GDT（6 项） |
| `0x93020..0x93088` | TSS（`esp0/ss0`） |
| `0x100000..0x1FEFFF` | 用户物理页池，255 页位图分配 |
| `0x400000` / `0x401000` | 用户 code RX / data RW |
| `0x402000` | guard：VMA 存在但无 PTE |
| `0x403000` / `0x404000` | 用户 stack RW（栈顶 `0x404000`）/ control R |
| `0x405000..0x410000` | 用户 heap，`brk` 从这里增长 |
| `0x20000` | VGA 影子显存 |
| `0x30000` | 串口帧快照 |
| `0xB8000` | VGA 文本显存 |

`boot.asm` 先读 128 个扇区，再读 64 个扇区到 `0x80000`，合计 192 个扇区，因此 `kernel.bin` 上限为 98,304 字节。
当前 P9 clean 构建 77,160 字节，BSS 从 `0x92D80` 到 `0x96DFC`；镜像余量 21,144 字节，BSS 距 `AEVUM_KERNEL_END=0x97000` 余 0x204（516B）。`kernel.bin` 结束于 `0x92D68`，BSS 尾部仍在第二段读取窗口 `0x90000..0x98000` 内，loader 按 `__bss_end` 清零。BSS 与 `render` 栈 `0x9000`、VGA `0xB8000`、VGA BIOS 影子 ROM `0xC0000` 均不重叠。任务槽从 P8 的 100B 增至 104B（新增 `zombie_ticks`），16 槽共增加 64B；BSS 只剩约 516B，继续加字段前必须先算预算。
`kernel.bin` 由 `kernel.elf` 经 `objcopy -O binary` 生成，保证 loader 与 C 看到的 `__bss_start`/`__bss_end` 一致。
若继续增长，先加 `boot.asm` 扇区数，同时同步 `kernel.ld` 与 `config/memory.h` 的 `AEVUM_KERNEL_END`，并确认 BSS 不越过内核主线程栈底 `0xC000`。`AEVUM_TASK_STACK_MIN=0x8000`，
`AEVUM_TASK_STACK_MAX=0x20C000`，`AEVUM_KSTACK_COUNT=12`，`AEVUM_TASK_STACK_SIZE=0x1000`，`AEVUM_AS_MAX=12`，`PROC_MAX=12`，`TASK_MAX=16`，`AEVUM_PHYS_POOL_BASE=0x100000`，`AEVUM_PHYS_POOL_END=0x1FF000`。

## 五、验收

```sh
make clean && make all
wc -c kernel.bin          # 必须 <= 98304
timeout 10 qemu-system-i386 -accel tcg -drive format=raw,file=disk.img \
  -display none -serial file:/tmp/robot-serial.bin -no-reboot
xxd -l 4 /tmp/robot-serial.bin   # 期望 a5 5a 01
```

通过标准：

- `kernel.bin` 不超过 98,304 字节，链接无 undefined symbol；P9 clean 构建 77,160 字节。
- QEMU 不 triple fault，串口首帧为 `a5 5a 01 ...`。
- 屏幕出现机械臂、目标点、轨迹和右侧 `ARM STATUS` 面板；标题显示 `PIT 100Hz | P/B/T/C/F/G/V/K`，面板出现 `px` 抢占计数和每任务 CPU 百分比。
- 空格扫描码 `0x39` 可切换零空间；每 420 帧自动切换。
- 按 `P`（扫描码 `0x19`）创建 `probe` 任务，画面显示 PID（首次为 4）和 `PIT PREEMPTIONS` 计数；探针循环不调用 `task_yield`，计数仍持续增长，证明 IRQ0 真的抢占了 CPU 密集型任务。任务退出后 `P3/31` 回到 3，再按 `P` 仍分配 PID 4，证明 ZOMBIE 回收与 PID 复用。
- 按 `B`（扫描码 `0x30`）让 `idle` 任务执行 `task_block()`，面板第三行从 `3id r` 变成 `3id B` 并出现 `BLOCKED` 提示；再按 `B` 调 `task_wake()`，变回 `3id r`，证明 `RUNNING -> BLOCKED -> READY` 两条边成立。
- 按 `T`（扫描码 `0x14`）进入全屏 `PREEMPTION DUEL`：创建 `spinA` / `spinB` 两个 CPU 密集型任务，循环里都不调用 `task_yield()`；两条 lane 的 `STEP` 同时增长，80 格抢占磁带出现青/洋红交替条纹，`PIT PREEMPTIONS` 持续增加。再按 `T` 停止，两个任务经 `task_exit()` 退出，画面恢复机械臂，面板回到 `P3/31`。
- 按 `C`（扫描码 `0x2E`）进入全屏 `AEVUM / CPU TIME ACCOUNTING`：三行任务的 `CPU%` 应稳定在约 `50% / 38% / 12%`，`TICKS` 每秒增长，`PIT TICKS` 等于三行 `TICKS` 之和；再按 `C` 返回机械臂，帧号和 `sw` 继续增长。
- 按 `F`（扫描码 `0x21`）进入全屏 `AEVUM / FPU CONTEXT LAB`：`fpuA` / `fpuB` 两个 CPU 密集型任务同时推进 `COUNT`，`TAG` 始终分别为 `A11CE` / `B22CE`，两条 lane 都显示 `CTX OK`；`SWITCHES` / `PREEMPTIONS` 持续增长。再按 `F` 停止，画面恢复机械臂。
- 按 `G`（扫描码 `0x22`）进入全屏 `AEVUM / PROCESS FAMILY TREE`：`famP` 下面先出现 `famA` / `famB`；约 1.5 秒后 `famP` 退出，两个子进程移到 `idle` 下面，右侧 `LAST REPARENT` 显示 `parent`、`children`、子 PID 和 `reaper`。再按 `G` 停止，画面恢复机械臂。
- 按 `V`（扫描码 `0x2F`）进入全屏 `AEVUM / V PAGE OBSERVABILITY / P5.2`：默认 `page 1/4 SLOTS`，10 行窗口显示 `window 1-10/12`，`SLOT 0..9` 同时出现；选中槽显示 KSTACK/CR3/BRK/VMA、image/argc/envc/switches/ticks、信号、children 与池统计。P10 抓屏中父 wait 前 `SLOT 4 PID 8 PP 7 ST ZOMB EX 7`，wait 后同一槽变 `FREE`，`POOL proc 3/12 live 3 zomb 0 dead 0`，`exit 8 st 9` 来自 exit ring；这证明 PCB 已清零且退出摘要仍可读。
- A/B 的 `brk(0x406000)` 返回 `0x406000`；heap `0x405800` 写入 `HELP` 后 `peek=0x48454C50`、`poke=1`；越界 `brk(0x500000)` 返回 `0xFFFFFFFF`；`poke(guard 0x402000)` 返回 0。
- D 第一轮 fork 后父子共享用户页，子进程写 `CHLD` 到 `0x401038` 后 `exit(7)`；父进程 wait 后读回 fork 前写的 `PAPA`，`0x401040` 写 `COW` 魔数，页面显示 `WAIT OK 7 / COW OK`。第一轮 COW 行为 `forks 2 shared 8 live 2 breaks 4 last AS4 old 00115000 -> 00118000`。
- `userC` 的 `mode=1` 数到 5000 后写 `0xB8000`，`userF` 的 `mode=3` 直接写 guard `0x402000`；C 行 `FAULT / SIG11`，F 行 `GUARD / SIG11`，`#PF` 面板 `pid6 cr2 000B8000 err 00000007 count 3`；A/B 的计数器继续增长，证明故障只杀死当前用户进程。
- `userD` 第二轮 `fork2` 返回 8；子进程先 `sigaction(SIGFPE,handler,0)`，再 `1/0` 触发 `#DE`，signal frame 进入 handler、`sigreturn` 返回 `0x40021D`，父进程 `wait 8` 得到 `st 8000000B`，页面显示 `HAND OK`；信号面板显示 `#DE 1 pid 8 eip 0040021B sig 8 SIGFPE hdl 1 chld 2 pend 00000000`。
- 再按 `V` 停止所有用户进程并回机械臂；机械臂 q 值保持正常，证明启动/调度/FPU 状态没有被 `#DE`、`#PF` 或 COW 路径破坏。再按一次 `V` 能重新创建 A/B/C/D/F，ASID 从 8 起、PID 仍复用 4-9，`PHYS PAGES` 保持 `20/255`，COW 统计累计为 `forks 4 shared 16 live 2 breaks 9`，没有 ZOMBIE/PID/页泄漏。
- `V` 页数字 `2` 切到 `page 2/4 TREE`：`roots 5 used 6/12 live 3 zomb 1 dead 2`，`PID 7 userD CH 1 W 1`，其下 `+1 PID 8 PP 7 ST ZOMB`；树按真实 PPID 递归，`W` 是 ZOMBIE 子进程数，不是静态文本。
- `V` 页数字 `3` 切到 `page 3/4 ADDRESS`：`userA CR3 00100000 ASID 1 VMA 6`，6 条 VMA 从 `00400000..00406000`；CODE `R-X PTE R-`、DATA/STACK/HEAP `RW- PTE RW`、GUARD `--- PTE --`、CTRL `R-- PTE R-`。底部 `TEST wait OK cow OK hand OK exec OK pipe OK`，以及 `A brk 00406000 poke 1 peek 48454C50 guard 0 bad FFFFFFFF`。
- `V` 页数字 `4` 切到 `page 4/4 FD/SIG`：fd0 `CONR kind 3`、fd1 `CONW kind 4`、fd2..7 closed，`OPEN fd 2/8`；`EXC #DE cnt 1 pid 8 eip 00400606 sig 8 SIGFPE hdl 1`；`#PF pid 7 eip 004002A4 cr2 00401080 err 00000007 count 8`；`PIPE obj 0`。
- `V` 页 `PgDn` 后窗口变 `3-12/12`，`SLOT 2..11` 全部出现；再 `Up` 两次选中槽 4，详情仍显示 `TASK ZOMB ... AS CR3 0011A000 ASID 5`。空槽详情显示 `SELECTED slot 6 EMPTY; use UP/DOWN to a used PCB slot`，底部仍保留 `POOL/PHYS/SCROLL` 统计。
- P5.2 clean 构建 `kernel.bin=64,872`、`boot.bin=512`、`disk.img=1,474,560` 字节；SHA256 分别为 kernel `48b046eb...`、boot `d9a70ce2...`、disk `97379832...`。增量构建与 `make clean && make all` 的 kernel 哈希一致，证明截图对应的是 clean 产物。
- P5.3 clean 构建 `kernel.bin=64,808`、`boot.bin=512`、`disk.img=1,474,560` 字节；SHA256 分别为 kernel `a39a6bd5...`、boot `d9a70ce2...`、disk `b1490110...`。`__bss_end=0x9395C`，`AEVUM_KERNEL_END=0x94000`；两轮 QEMU 压测截图与文本均来自该 clean 产物。
- 按 `S`（扫描码 `0x1F`）进入 `AEVUM / P5.3 FORK/WAIT STRESS`：第一轮 `round 100/100 first 5 -> 5 live 1 state STRESS OK`，`PHYS 6/255 proc 1/12 AS 1/12 KSTK 1/12`；按 `S` 停止回主画面，再按 `S` 第二轮仍为 `round 100/100`、首末 PID `5 -> 5`、`PHYS 6/255`、`proc/AS/KSTK 1/12`，ASID 从 1 变为 102。两轮都完成 100 次 `wait` 且 PID 复用，证明子进程回收后 PID/AS/内核栈/物理页不泄漏。
- P6.0 clean 构建 `kernel.bin=64,488`、`boot.bin=512`、`disk.img=1,474,560` 字节；SHA256 分别为 kernel `48b25c3f...`、boot `d9a70ce2...`、disk `bbcc9235...`。`__bss_end=0x9387C`，`AEVUM_KERNEL_END=0x94000`。
- P7.0 `spawn` clean 构建与 QEMU 验收：D lane 在四轮 fork/wait 后调用 `eax=18 spawn(1, argv, envp)`，子进程不复制父 AS，独立 AS/PCB/内核栈/用户栈，`exec(1)` 后 `exit(10)`，父 `wait` 得到 10；V 页 TEST 行显示 `spawn OK`。
- P7.1 clean 构建 `kernel.bin=70,568`、`boot.bin=512`、`disk.img=1,474,560` 字节；SHA256 分别为 kernel `0E963C6BB20E9F3E8BA6ADC61BE3C4CB3FC4E685D54C396062D23A14CACF281B`、boot `218ECC892630F5F723B8E3228898714B18D82BF31A4AD31071F9A3480B6CE3D2`、disk `FE69DF086AC6B54D41E5A37E8FE79C98785EF66E994AF26D362B6475BBDF8C2`。`__bss_start=0x913C0`、`__bss_end=0x952BC`，`AEVUM_KERNEL_END=0x96000`；增量构建与 `make clean && make all` 的 kernel 哈希一致。
- P7.1 QEMU 验收：V 页 ADDRESS 显示 `TASK READY ... THR 1 TID 7`、`THREAD tid 6 join 0 shared 1 THRD`；TEST 行 `wait OK cow OK hand OK exec OK pipe OK spawn OK thr OK`。线程组 PID 7，主线程 TID 7，worker TID 6；worker 对共享计数器加 1 后 `thread_exit(0)`，主线程 `thread_join(6, status)` 返回 0、status 0，回收后线程任务槽/内核栈归还，只剩 `THR 1`。
- P7.1 边界：只验证正常 clone/exit/join；尚未覆盖主线程先退、worker 继续、重复 join、join 自己、worker `#PF/#DE`、多线程 `exec` 拒绝返回值和 `process_stop_all()` 的线程组停止语义。`clone` 只接受 `VM/FILES/SIGHAND/THREAD`，没有 `CLONE_FS`、TLS、futex、detach 或 per-thread signal pending。
- P7.2 clean 构建 `kernel.bin=71,272`、`boot.bin=512`、`disk.img=1,474,560` 字节；SHA256 分别为 kernel `0BBD860EEA0A89E2DE00A4431CF56629DAA1426772867216AA03AA98B6C7BF27`、boot `218ECC892630F5F723B8E3228898714B18D82BF31A4AD31071F9A3480B6CE3D2`、disk `43D37B6F70507E30A9A8CA1A1D392D6E86C83AAC1FF734FAA60D0F1716ECA10A`。`__bss_start=0x91680`、`__bss_end=0x955BC`，`AEVUM_KERNEL_END=0x96000`；`.text` 结束 `0x8F78E`，`.rodata` `0x8F7A0..0x91568`，`.data` `0x91580..0x91668`，`kernel.bin` 结束正好 `0x91668`。
- P7.2 QEMU 验收：V 页 ADDRESS 显示 `THREAD tid 6 join 0 shared 1 THRD det OK`；TEST 行 `wait OK cow OK hand OK exec OK pipe OK spawn OK thr OK det OK`；`TASK READY ... THR 1 TID 7`。第二个 worker 从 `eax` 取共享计数器地址，`gettid` 后自 `thread_detach`，返回 0；它写计数器 1、置完成标记后 `thread_exit(0)`。主线程对 detached TID 调 `thread_join` 得到 -1，写 `DETD=0x44544544`；任务槽由 `task_reap_detached()` 延后回收，页面只剩主线程。
- P7.2 边界：detach 只验证 self-detach 的正常退出；最后一个活线程是 detached 线程时的行为、重复 detach、主线程 detach 其他 worker、detached 线程故障终止整组、以及 `process_stop_all()` 对 detached 线程的停止语义仍未单独压测。
- P7.3 clean 构建 `kernel.bin=72,040`、`boot.bin=512`、`disk.img=1,474,560` 字节；SHA256 分别为 kernel `73A439C9626C1EAE65C55732ECFBDF04FE31FEA2EE835C6978DE6660ACBC9D33`、boot `218ECC892630F5F723B8E3228898714B18D82BF31A4AD31071F9A3480B6CE3D2`、disk `BF5E3A8D8A8A3355F0D6A86E45F003328AAF1D5C8F0FA679624FDEFAFBF51C3D`。`__bss_start=0x91980`、`__bss_end=0x9587C`，`AEVUM_KERNEL_END=0x96000`；`text 71,784`、`data 232`、`bss 16,124`。
- P7.3 QEMU 验收：V 页 ADDRESS 显示 `THREAD tid 6 join 0 shared 1 ok THRD det OK main OK arg OK`；TEST 行 `wait OK cow OK hand OK exec OK pipe OK spawn OK thr OK det OK`；`TASK READY ... THR 1 TID 7`。fork 子进程主线程先 `thread_exit(0)`，worker 继续运行、`thread_join` 主 TID 得到 status 0，再以 `exit(7)` 结束；父进程 wait 返回子 PID、status 7，写 `MTHR`。`EDGE` 覆盖 join/detach 的 0、-1、self、99。
- P7.3 边界：尚未覆盖跨线程组 join 一个真实存在的 TID、detach 已 ZOMBIE 线程、主线程 detach 自己后退出、主线程 ZOMBIE 留在组内由最后线程触发整组 `process_reap()`、以及 detached 线程故障终止整组。
- P7.4 clean 构建 `kernel.bin=72,744`、`boot.bin=512`、`disk.img=1,474,560` 字节；SHA256 分别为 kernel `B7473DDC26D05F1F25273B36DFFB13CBD956BFCE4EE77AA4CB96852926C053B4`、boot `218ECC892630F5F723B8E3228898714B18D82BF31A4AD31071F9A3480B6CE3D2`、disk `801EC6310ED509530C3C347A9CF764B096E74B9074F84E06BE006E82D9083DAA`。`__bss_start=0x91C40`、`__bss_end=0x95B3C`，`AEVUM_KERNEL_END=0x96000`；`.text` 64,839、`.rodata` 7,624、`.data` 232、`.bss` 16,124。
- P7.4 QEMU 验收：V 页 ADDRESS 显示 `THREAD tid 6 join 0 shared 1 ok THRD det OK main OK arg OK grp OK`；TEST 行 `wait OK cow OK hand OK exec OK pipe OK spawn OK thr OK det OK`；`TASK READY ... THR 1 TID 7`。子进程主线程创建 A/B 两个 worker 后不 join、以 ZOMBIE 留在组内；A 退出 5，B join A 得到 status 5 后作为最后线程退出 7，父 `wait` 得到 7，`process_reap()` 释放整组，tasks 表中该子进程 3 个任务槽全部消失。
- P7.4 边界：未覆盖 worker B 不 join A、A 退出后以 ZOMBIE 留到整组回收的子变体；未覆盖 detached worker 故障终止整组、主线程 ZOMBIE 被单独 detach；多线程 `exec` 仍拒绝。`wait` / `thread_join` 的阻塞语义由 P7.5 接续。
- P7.5 clean 构建 `kernel.bin=73,992`、`boot.bin=512`、`disk.img=1,474,560` 字节；SHA256 分别为 kernel `d4517d1595d6250126f69c39d6170c3b65f371bae0ef8480ab2eb49867cd6d95`、boot `218ecc892630f5f723b8e3228898714b18d82bf31a4ad31071f9a3480b6ce3d2`、disk `e99cb5b6f7eb2ae478864ea0622fd6493daef793d3794a8ef7a4be436dca5c2e`。`__bss_start=0x92120`、`__bss_end=0x9601C`，`AEVUM_KERNEL_END=0x97000`；`.text` 66,063、`.rodata` 7,656、`.data` 232、`.bss` 16,124；`kernel.bin` 结束于 `0x92108`。ring3 用户程序 `0x80110..0x810EF` 共 4,063B，低于 4KB code VMA。
- P7.5 QEMU 验收：V 页 ADDRESS 第 15 行 `EXEC ... THR 1 TID 7 blk 7 /1`；第 21 行 `TEST wait OK cow OK hand OK exec OK pipe OK spawn OK thr OK det OK blk OK`；`THREAD tid 6 join 0 shared 1 ok THRD det OK main OK arg OK grp OK`。父 `wait` 在子 LIVE 时阻塞，子主线程 `thread_join` 在 worker LIVE 时阻塞；worker 退出 4、子写 `BLK5` 后退出 9，父被唤醒得到 status 9 并写 `WBLK`。tasks 表只剩 control/render/idle/userA/userB/userD 六个任务槽，P7.5 子进程主线程与 worker 均已回收；P7.4 的 `grp OK` 路径仍通过。
- P7.5 边界：已覆盖单父、单子、单 worker 的正向阻塞/唤醒；未覆盖 `wait`/`join` 超时、信号 EINTR、多个等待者同时等同一目标、TID/PID 复用竞态。第一版把“重查 + 置 BLOCKED”放在同一 `cli` 窗口，唤醒只在任务真实 ZOMBIE 后发生，以此作为 lost wake 的唯一防线。
- P8 clean 构建 `kernel.bin=75,336`、`boot.bin=512`、`disk.img=1,474,560` 字节；SHA256 分别为 kernel `5198943bab94bbf3aa630b67285a33fd00851dfa761ea39392abc3439b039c8d`、boot `218ecc892630f5f723b8e3228898714b18d82bf31a4ad31071f9a3480b6ce3d2`、disk `7ea3bdcb7e1e8eca63814ab88cd35b0f2bf698afcef74faeb47b127b1aea56e6`。`__bss_start=0x92660`、`__bss_end=0x9669C`，`AEVUM_KERNEL_END=0x97000`；`.text` 67,420、`.rodata` 7,656、`.data` 232、`.bss` 16,444；`kernel.bin` 结束于 `0x92648`。ring3 用户程序仍为 `0x80110..0x810EF` 共 4,063B，无 code VMA 变化。
- P8 QEMU 验收：机械臂主页面右下显示 `kthr 4 s5 OK`，`TASK SCHED P3/31`；`kping` 运行时 TID 4，退出码 5，join 前 control 在 `task_block_until()` 进入 BLOCKED。tasks 数组 dump 只剩 control/render/idle 三个槽，slot 3 全零，TID 4 已归还；kthread 计数 `created=1 exited=1 reaped=1 join_blocks=1 join_wakes=1 failed=0`，动态栈使用量在创建前/join 后都为 0。
- P8 边界：只做单创建者、单 joiner、无 detach 的最小闭环。未 join 的 kthread ZOMBIE 泄漏由 P9 补上；多个 joiner 等同一 TID、join 超时/取消、TID 复用竞态、kthread 故障终止、worker 池和高频创建/销毁压测仍未覆盖。`kthread_create` 只允许 ring0 任务调用；kthread 不创建 PCB，也不进入用户线程组。
- P9 clean 构建 `kernel.bin=77,160`、`boot.bin=512`、`disk.img=1,474,560` 字节；SHA256 分别为 kernel `e929264092c1725186393e67a1b3b32e4d537455d53c1aa50f0d1265f4a6ef8d`、boot `218ecc892630f5f723b8e3228898714b18d82bf31a4ad31071f9a3480b6ce3d2`、disk `8744b4b8b133e10296a225cab802a5725769973acc053b512bf42f08d5c71f5a`。`__bss_start=0x92D80`、`__bss_end=0x96DFC`，`AEVUM_KERNEL_END=0x97000`；`.text` 69,210、`.rodata` 7,700、`.data` 232、`.bss` 16,508；`kernel.bin` 结束于 `0x92D68`。ring3 用户程序仍为 `0x80110..0x810EF` 共 4,063B。
- P9 QEMU 验收：clean 构建后 `timeout` 启动 QEMU，串口首帧仍为 `a5 5a 01`，主页面右下 `kt 4 s5 a1 OK`，`TASK SCHED P3/31`。`s5` 是 `kping` 正常 join 的退出码，`a1` 是 `korph` 的自动回收次数；页面 `OK` 来自 `kthread_demo_ok`，它同时要求 `reaped==2`、`auto_reaped==1`、`live==0`、`join_blocks>0`，且 `kernel_stack_used()` 回到创建前。
- P9 边界：grace 是 100 tick 固定值，晚于 grace 才 join 的调用者得到 -1；这次只验证单创建者、无 joiner 的自动回收，没有覆盖多个 joiner、join 超时/取消、TID 复用竞态、kthread 故障终止和高频创建/销毁。自动回收在调度器切换路径上扫 16 个任务槽，未做单独延迟测量；BSS 距 `AEVUM_KERNEL_END` 只剩 516B，继续加字段前必须先算预算。
- G 页按 `G` 进入后约 3 秒抓屏，`LAST REPARENT` 显示 `parent 4 / exited / children 2 / famA ->3 / famB ->3 / reaper 3`；树中 `famA`/`famB` 已挂到 `idle` 下面，说明 4 行显示与 reaper 下移没有破坏现有两子进程场景。
- V 页 `2 TREE` 抓屏显示 `roots 5 used 6/12 live 3 zomb 1 dead 2`，底部 `ORPHAN none yet` 不覆盖 TREE 统计和第 23/24 行公共底栏；当前 demo 没有父进程先退出留子的 lane，所以只验证了空事件布局，事件写入路径由 `process_exit_current/process_terminate_current` 的真实退出分支调用。


- 在机械臂主画面按 `K`（扫描码 `0x25`）：页面切到 `AEVUM / KERNEL PAGE FAULT`，显示 `task 2 render`、`CS=0x0008 / CPL 0`、`EIP=0x000837C1`、`CR2=0x00400000`、`ERR=0x00000002`、`CR3=0x00010000`、`P=0 W/R=1 U/S=0 RSVD=0 I/D=0`、`fault count 1`、`recoveries 0` 和 `kernel image 0x00080000..0x0008F400`；按 `R` 后回到机械臂，q 值继续变化。先跑完整 V 后按 `K`，累计 `fault count 6`：V 页自身 3 次、V 停止路径额外 2 次、K 探针 1 次；这个 count 不是单次 probe 次数。按 `H` 才停机。
