# 进程上下文完整化路线

本文主体讨论 ring3 用户进程，不把 `control/render/idle` 这三个静态内核任务当成完整进程；P8/P9 的动态内核线程单独列为 ring0 调度实体，它没有 PCB/线程组，生命周期由 `kthread_create/exit/join` 和 grace 兜底回收管理。

P1 建立用户进程 PCB、PID、父子关系、退出码和 `fork/exit/wait/reap` 闭环。P2.1 把地址空间推进到“255 页物理池 + 6 个 VMA + brk + guard + copyin/copyout”。P2.2 完成 COW fork 与物理页引用计数。P3.1 完成用户同步异常的默认终止、信号 pending/SIGCHLD、wait status 编码和 V 页信号视图；P3.2 在默认终止之上加入每进程 handler 表、用户栈 signal frame、`sigreturn`，并修复第二次 fork 时已有 COW 页丢失 COW 位的问题；P4.1 加入内嵌 flat 镜像 `exec`，保留 PID/任务槽，替换用户 AS、入口、栈和信号状态；P4.2 给 `exec` 增加 `argv/envp` 解析、字符串迁移和 SysV 初始栈；P4.3 增加每进程 fd 表、匿名 pipe、`pipe/read/write/close`，并证明 exec 后 fd 表继续有效；P4.4 把 fd 0/1 接到 console 字符设备，验证键盘输入、VGA/COM1 输出、fork 继承和 close 边界。P7.0 增加 `spawn(image_id,argv,envp)`：不经过 fork，直接为当前进程创建独立 AS/PCB/内核栈/用户栈的子进程，D lane 用 image 1 子进程和 wait(10) 做闭环。P7.1 增加最小线程组：`clone` 只共享 VM/FILES/SIGHAND/THREAD，`task` 拥有独立 TID/内核栈/用户栈/FPU，`thread_exit` 只结束当前任务，`thread_join` 回收同组任务，最后一个活线程退出才让进程进入 ZOMBIE。P7.2 增加 `thread_detach`：detached 线程退出后由调度器延后回收任务槽/内核栈，之后 `thread_join` 返回 -1。P7.3 验证主线程先退、worker 继续并 join 主线程，同时把所有非法 join/detach 参数压回 -1。P7.4 验证主线程不 join、以 ZOMBIE 留在组内，worker A 退出后 worker B 作为最后活线程退出，由父 `wait` 的 `process_reap()` 一次性回收整组任务槽。P7.5 把 `wait` / `thread_join` 从 `task_yield_wait()` 轮询改成真正的 `BLOCKED -> WAKE`：在 `cli` 窗口内重查条件后写 `TASK_BLOCKED` 并进门，调度器只在任务已实际转 ZOMBIE 后唤醒同组 join 等待者和父进程组，避免 lost wake。P8 把同样的阻塞/唤醒语义用到 ring0 动态内核线程：`kthread_create` 运行时申请任务槽、TID 和栈池 4KB 栈，`kthread_exit` 只结束当前任务，`kthread_join` 等到目标真正 ZOMBIE 后读退出码并释放动态栈/TID/任务槽；`task_reap()` 跳过 kthread，静态 control/render/idle 仍使用固定栈。P9 补上未 join kthread 的泄漏兜底：ZOMBIE 后记录 `zombie_ticks`，过 100 tick grace 且没有任何任务在 `kthread_wait_tid` 上等它时，由调度器回收动态栈/TID/任务槽。P11.1 把阻塞等待补上超时：`task_struct` 增加 `wake_tick`，PIT 每 tick 扫描到期 BLOCKED 任务；新增用户态 `eax=24 sleep(ticks)` 与 `eax=25 wait_timeout(pid,status,ticks)`，条件满足优先于超时。P11.2 增加优先级生命周期：`eax=26 getpriority(tid)`、`eax=27 setpriority(tid,priority)`，`tid=0` 表示当前任务，0 最高、3 最低；用户态只能操作 user 任务，fork 子进程和 clone 线程继承调用者优先级。P11.3 增加对象等待队列与带 tick 上限的阻塞 I/O：保留 `eax=15 read` / `eax=16 write` 的非阻塞语义，新增 `eax=28 read_block` / `eax=29 write_block`；pipe 读写与 console 输入各挂等待队列，`task_block_on()` 在关中断窗口内重查条件、关中断入 BLOCKED，超时由 PIT tick 唤醒。

## 一、P3.1/P3.2/P4.1/P4.2/P4.3/P4.4/P5.1/P5.2/P5.3/P6.0/P7.0/P7.1/P7.2/P7.3/P7.4/P7.5/P8/P9/P10/P11.1/P11.2/P11.3 现在的实现

| 能力 | 当前实现 | 位置 |
| --- | --- | --- |
| 用户同步异常 | `#DE/#BP/#UD/#GP` 由 `exception.c` 唯一分发；`#PF` 仍由 `page_fault.c` 独占 | `arch/x86/cpu/exception/exception.c` |
| 异常入口 | `exception_de/bp/ud/gp_stub` 统一压出与 `struct irq_frame` 对齐的帧；`#DE/#BP/#UD` 补 `err=0`，`#GP` 使用 CPU 压入的 error code | `loader.asm` |
| 异常映射 | `#DE -> SIGFPE(8)`、`#BP -> SIGTRAP(5)`、`#UD -> SIGILL(4)`、`#GP -> SIGSEGV(11)` | `exception.c` |
| 默认动作 | 没有 handler 时，CPL3 异常记录现场后 `process_terminate_current(sig)`，再 `task_exit()`；CPL0 非 #PF 异常进入诊断停机 | `exception_dispatch()` |
| 用户 handler | P3.2 每进程 `sigactions[32]`：handler、mask、flags；`0` 默认终止、`1` 忽略、其他值是用户 VA | `include/kernel/proc.h`、`process.c` |
| signal frame | P3.2 在用户栈低地址放 trampoline 返回地址，随后保存原 GPR 与 `eip/cs/eflags/esp/ss`、`sig/old_mask/magic` | `process_signal_deliver()` |
| sigreturn | P3.2 `eax=12` 从用户栈读回 frame，恢复 GPR/iret 帧和旧 mask；handler 用 `ret` 进入 trampoline | `process_sigreturn_current()` |
| handler 继承 | `fork` 复制父进程 `sigactions` 与 `sig_mask`，不复制 pending；`sig_handled` 从 0 开始 | `process_fork()` |
| 信号常量 | `SIGILL 4 / SIGTRAP 5 / SIGFPE 8 / SIGSEGV 11 / SIGCHLD 17` | `include/kernel/proc.h` |
| P3.2 系统调用 | `eax=11 sigaction(sig,handler,mask)`、`eax=12 sigreturn()`；复用 `int80_stub` 返回同一 `irq_frame` 的路径 | `arch/x86/cpu/syscall/syscall.c` |
| P4.2 exec | `eax=13 exec(image_id,argv,envp)`；`process_exec_current()` 先在旧 AS 解析两个向量，再建新 AS、构造 `argc/argv/envp` 新栈、切 CR3、更新任务 AS 指针、重置帧和信号状态、释放旧 AS；只接受镜像 1 | `kernel/proc/process.c`、`syscall.c` |
| P4.2 exec 元数据 | `struct process.image_id/exec_argc/exec_envc`：ring3=0、exec=1；`fork` 继承。P10 前 `reap` 保留墓碑，P10 后摘要进入 exit ring、PCB 清零；`process_image_id(pid)` 与 `process_exec_counts(pid)` 先查活 PCB，再回退最新 ring | `include/kernel/proc.h`、`process.c` |
| P4.2 任务 AS 指针 | `task_set_space()` 是 exec 更新任务持有 AS 的唯一入口；CR3 切换仍归 `address_space` | `kernel/task/task.c` |
| P4.3 fd 表 | PCB 内嵌 `struct proc_fd_table fds`，8 槽；fd 0 为 console 读、fd 1 为 console 写，pipe 从 2 起；`kind` 区分 console 与 pipe；`fork` 复制，pipe 端点 acquire、console 端点继承；exec 保留、exit/terminate/reap 关闭 | `kernel/proc/fd/fd.c`、`include/kernel/proc/fd.h` |
| P4.3 pipe | 4 个对象、每对象 64 字节非阻塞环形缓冲；`refs/readers/writers` 控制生命周期；空读有写端 `-1`、写端全关 `0`、满写 `-1` | `kernel/ipc/pipe/pipe.c` |
| P4.3 系统调用 | `eax=14 pipe(ebx=user u32[2])`、`15 read(ebx=fd,ecx=buf,edx=count)`、`16 write`、`17 close(ebx=fd)`；失败统一 `-1` | `syscall.c`、`fd.c` |
| P4.3 exec 镜像 2 | `user/exec-pipe/exec_pipe.asm` 从 `argv={"pipe","writer","FD=3"}` 取 fd，向继承写端写 `PIPE`，成功 `exit(12)`；失败码 26..31 | `process.c`、`user/exec-pipe/` |
| P4.4 console | `drivers/console/console.c`：16 字节 ASCII 输入队列、8×64 输出行缓存、COM1 镜像、scan/push/drop 统计；`read(0)` 非阻塞，`write(1)` 最多 64 字节 | `drivers/console/console.c` |
| P4.4 fd 0/1 | 新用户进程创建后安装 fd0 读、fd1 写；`fork` 复制父表，`exec` 保持，`close` 只清本进程句柄；console 端点不参与 pipe 引用计数 | `kernel/proc/fd/fd.c`、`process.c` |
| P4.4 X 页 | mode 4 父进程 fork 后关闭自身 0/1，子进程继续读键并写 `CON:<key>` / `FD01 OK`，`exit(14)`；页面显示 close 结果、输入队列、输出行和 UART 字节 | `app/console/demo`、`app/console/render` |
| P7.0 spawn | `eax=18 spawn(image_id,argv,envp)`：当前 ring3 进程直接创建独立子进程，不复制父 AS；父 AS 解析参数，子进程新建 AS/内核栈/PCB，复制 fd 表后以 `0x400000` 入口发布；父 wait 回收。D lane spawn image 1，子进程 exit(10)，V 页写 `SPAWN OK` | `kernel/proc/process.c`、`arch/x86/cpu/syscall/syscall.c`、`user/ring3/ring3.asm` |
| P7.1 线程组 | `process` 是资源组，`pid` 为 TGID；`task` 是调度实体，`pid` 为 TID，新增 `tgid`。`task_create_thread_user()` 复用 `task_create_common()`，共享 `proc->space`，只分配任务槽、TID、内核栈；线程栈顶由调用方提供 | `kernel/task/task.c`、`include/kernel/task.h` |
| P7.1 clone | `eax=19 clone(ebx=entry, ecx=user_stack, edx=flags, esi=arg)`；只接受 `PROC_CLONE_VM|FILES|SIGHAND|THREAD`（`0x00010D00`），不实现 `CLONE_FS`；共享当前 AS/fd/信号表，不分配 process 槽，不复制 AS/fd；初始帧 `eax=arg` | `kernel/proc/process.c`、`syscall.c` |
| P7.1 thread exit/join | `eax=20 thread_exit(code)` 只把当前任务置 ZOMBIE；最后一个活线程退出才 `process_exit_current()`。`eax=21 gettid()` 返回当前 TID。`eax=22 thread_join(tid,status)` 只接受同组、非当前、已 ZOMBIE 的线程，复制退出码后 `task_reap_thread()` 回收任务槽和内核栈 | `kernel/proc/process.c`、`kernel/task/task.c`、`syscall.c` |
| P7.1 线程组边界 | `process_exit_current/terminate_current` 先 `task_terminate_threads()` 再关闭 fd；`process_reap()` 用 `task_release_process_threads()` 释放整组任务槽；`process_alive()` 按活线程数；`process_exec_current()` 拒绝多线程 exec；`process_snapshot()` 优先返回非 ZOMBIE 线程并暴露 `tid/thread_count` | `kernel/proc/process.c`、`kernel/task/task.c` |
| P7.2 thread_detach | `eax=23 thread_detach(ebx=tid)` 只接受同组 TID；`task.detached=1` 后 `thread_join` 返回 -1；线程退出为 ZOMBIE 时，`task_reap_detached()` 在调度器下一次切换中回收非 current 的 detached 任务槽与内核栈；当前退出线程的栈等下一次切换再回收 | `kernel/task/task.c`、`kernel/proc/process.c`、`syscall.c` |
| P7.3 leader-first exit | 主线程 `thread_exit(code)` 时若 `task_process_live_threads(self)>1`，只把当前任务置 ZOMBIE，进程保持 LIVE；worker 继续被调度，可 `thread_join` 已退出的主 TID，复制 status 后回收其任务槽/内核栈；最后活线程退出时才走 `process_exit_current()`。非法参数矩阵 `join(0/-1/self/99)` 与 `detach(0/-1/99)` 全部返回 -1 | `kernel/proc/process.c`、`kernel/task/task.c`、`syscall.c` |
| P7.4 group reclamation | 主线程创建两个 worker 后不 join 直接 `thread_exit(0)`，主任务以 ZOMBIE 留在组内；A 退出 5，B join A 得到 status 5 后作为最后活线程 `thread_exit(7)`，触发 `process_exit_current()`；父 `wait` 得到 7 后 `process_reap()` 调 `task_release_process_threads()` 一次性释放组内所有任务槽/内核栈，包括未 join 的主 ZOMBIE | `kernel/proc/process.c`、`kernel/task/task.c`、`user/ring3/ring3.asm` |
| P7.5 block/wake | `task_block_until(ready,ctx)` 在 `pushf; cli` 后重查 `ready(ctx)`：已满足则恢复 flags 返回 0；否则写 `TASK_BLOCKED`、`SCHED_BLOCK` 后 `int $0x40`，唤醒返回 1，状态/调度失败 -1。`process_wait()` 与 `process_thread_join_current()` 使用各自的 `proc_wait_ready/proc_join_ready` 在外层继续扫描；调度器在 `prev->state == TASK_ZOMBIE` 后调 `process_notify_task_zombie()`，先 `task_wake_process(proc)`，进程整体 ZOMBIE 时再唤醒父进程组。全局 `wait_block_total/wait_wake_total/join_block_total/join_wake_total` 只统计 wait/join 的实际阻塞与唤醒 | `kernel/task/task.c`、`kernel/proc/process.c`、`include/kernel/task.h`、`include/kernel/proc.h` |
| P8 dynamic kthread | `kthread_create()` 从 `kernel_stack_alloc()` 取 4KB 动态栈，经 `task_create_kernel_arg()` 建带 `kthread=1` 的 ring0 任务并保存 fn/arg；`kthread_exit(code)` 置退出码后 `task_exit()`。`task_reap()` 跳过 kthread ZOMBIE，`kthread_join(tid,&status)` 先登记 `kthread_wait_tid`，再用 `task_block_until()` 等目标 ZOMBIE；调度器在 `prev->kthread` 转 ZOMBIE 后按 TID 唤醒 joiner。join 读出 status 后由 `task_reap_kthread()` 释放动态栈、PID 和任务槽。kthread 不创建 PCB，不进入 process 线程组 | `kernel/task/kthread/kthread.c`、`kernel/task/task.c`、`include/kernel/kthread.h` |
| P9 kthread 泄漏兜底 | 任务转 ZOMBIE 时记录 `zombie_ticks`；调度器每轮调 `kthread_reap_unjoined()`，`task_reap_unjoined_kthread(now,100)` 跳过 current、未满 grace 和有 `kthread_wait_tid` 等待者的目标，其余释放 4KB 动态栈、TID、任务槽。`kthread_stats` 增加 `auto_reaped`，`reaped` 是 join+兜底总回收数，`live=created-reaped`。唤醒 joiner 时保留 `wait_tid`，`kthread_join` 在 reap 成功后才清 | `kernel/task/task.c`、`kernel/task/kthread/kthread.c`、`include/kernel/kthread.h`、`include/kernel/task.h` |
| P10 PCB 清零 | `struct process` 删除每进程 `reaped[2]`，改为全局 `exit_ring[4]`；`process_reap()` 先保存 `pid/ppid/exit_code/term_sig/image_id/argc/envc/comm`，再 `proc_zero()`，PCB 槽真正回到 `PROC_FREE`，不再保留 DEAD 墓碑。`process_reaped_exec()`、`process_image_id()`、`process_exec_counts()` 和 `proc_reusable_child_pid()` 都改从 ring 读取 | `kernel/proc/process.c`、`include/kernel/proc.h`、`app/user/render/slots.c` |
| P11.1 定时阻塞 | `task_struct` 增加 `wake_tick`；`task_block_until_timeout(ready,ctx,ticks)` 进入时固定 deadline，`cli` 窗口内先查条件、再查 `(int)(tick_total-deadline)>=0`，否则写 BLOCKED + wake_tick 后进门；PIT IRQ0 每 tick 调 `task_wake_expired()` 把到期任务改 READY。`task_sleep_ticks()` 导出 `eax=24`，`process_wait_timeout()` 导出 `eax=25`；`ticks=0` 只做一次非阻塞查询 | `kernel/task/task.c`、`kernel/proc/process.c`、`arch/x86/cpu/syscall/syscall.c`、`user/ring3/ring3.asm` |
| P11.2 优先级生命周期 | `task_priority_get(tid)` / `task_priority_set(tid,priority)` 只改后续 PIT 记账权重，不重算历史 `vruntime`；0..3 越界返回 -1，ZOMBIE 不接受 set，用户态不能改 control/render/idle。`eax=26 getpriority(ebx=tid)` 返回 0..3/-1，`eax=27 setpriority(ebx=tid,ecx=priority)` 返回 0/-1；`tid=0` 取当前任务。fork 继承父任务优先级，clone 线程继承当前线程优先级，D lane 验证父 P1、子 P3、clone P2 | `kernel/task/task.c`、`kernel/proc/process.c`、`arch/x86/cpu/syscall/syscall.c`、`include/kernel/task.h`、`user/ring3/ring3.asm` |
| P11.3 对象等待队列 / 阻塞 I/O | `struct wait_queue { u8 wake_seq; }`；`task_block_on(q,ready,ctx,ticks)` 在 `cli` 窗口内先重查条件、再查 deadline，才置 BLOCKED + `wake_tick` + `waitq`；`wait_queue_wake_all()` 扫 16 槽把 `waitq==q` 的 BLOCKED 任务改 READY，只改状态不切任务。pipe 增加 `read_wq/write_wq`，console 输入增加 `input_wq`；`proc_fd_read_block/write_block` 做用户范围校验与 copyin/copyout。新增 `eax=28 read_block(ebx=fd,ecx=buf,edx=len,esi=ticks)`、`eax=29 write_block`：成功字节数、EOF 0、错误 -1、超时 -2，`ticks=0` 等价非阻塞；旧 `eax=15/16` 语义不变。image 3 独立镜像 @ 0x400000 验收三段阻塞路径 | `include/kernel/task.h`、`kernel/task/task.c`、`kernel/ipc/pipe/pipe.c`、`drivers/console/console.c`、`kernel/proc/fd/fd.c`、`arch/x86/cpu/syscall/syscall.c`、`user/exec-io/exec_io.asm` |
| wait status | 正常退出保留原退出码（`exit(7) -> 7`）；信号终止为 `0x80000000` 按位或 `sig`（`#DE -> 0x80000008`） | `PROC_WAIT_STATUS()` |
| PCB 信号字段 | `pending/last_sig/term_sig/sig_count/sigchld_count`；`process_info` 同步暴露 | `kernel/proc/process.c` |
| pending 位图 | `pending |= 1u << (sig - 1)`；只记录 1..31 号信号，不在这里改进程状态 | `proc_signal_pending()` |
| SIGCHLD | 子进程正常退出或被信号终止都 `proc_notify_parent()`；父进程 LIVE 时置 SIGCHLD pending | `proc_notify_parent()` |
| wait 消费 | `process_wait()` 取走 ZOMBIE 后清父进程 SIGCHLD pending；`process_reap()` 把退出摘要写进 exit ring 后立即清零 PCB，V 页改读 ring 的最近记录 | `process_wait()` / `process_reap()` / `process_exit_snapshot()` |
| 用户态 #PF | 不可恢复的 guard/RO/非法 #PF 改走 `process_terminate_current(SIGSEGV)`；C/F 显示 `SIG11` 而不是 `EXIT 142` | `page_fault.c` |
| PID 复用 | `process_fork()` 从 exit ring 取该父进程最近退出的直接子 PID，再 `task_pid_reserve_hint()`；不再依赖 DEAD 墓碑 | `proc_reusable_child_pid()`、`task_pid_reserve_hint()` |
| 调度记账 | 普通 `task_yield()` 不记 vruntime，保持演示与轮询推进速度；`process_wait()` / `thread_join` 改用 `task_yield_wait()`，先按 `WEIGHT_QUANTUM / weight` 记当前任务 vruntime 再切走，避免低 vruntime 的等待者饿死被等任务 | `kernel/task/task.c`、`kernel/proc/process.c` |
| V 页 | 标题 `AEVUM / COW FORK + SIGNALS + PHYS`；PCB 表回收后槽直接显示 `FREE`；`SLOT` 页底部 `exit <pid> st <status>` 读 exit ring 最新记录；`ADDRESS` 页 `REAP0/REAP1` 仍读该父进程最近两次回收 | `app/user/render/render.c`、`slots.c`、`address.c` |

槽位预算：`TASK_MAX=16`（3 个静态内核任务 + 动态 kthread/用户任务/线程 + 余量）、`PROC_MAX=12`、`AEVUM_AS_MAX=12`（exec 期间旧 AS、新 AS、fork 子 AS 同时存在）、`PID_MAX=31`；P7.1 的线程 TID 与进程 TGID 共用 1..31 的 `pid_map`，每个线程另占一个任务槽和一个 4KB 内核栈；P8 的 kthread 同样占一个任务槽、一个 TID 和一个 4KB 动态栈，但没有 PCB。P10 增加全局 `PROC_EXIT_HISTORY=4` 的 exit ring，每槽 48B，共 192B；同时删掉每 PCB 的 `reaped[2]`（12×32B），BSS 净减 192B。P4.3 另加 `PROC_FD_MAX=8`、`PROC_FD_FIRST=2`、`PIPE_MAX=4`、`PIPE_CAPACITY=64`；P4.4 加 `CONSOLE_INPUT_MAX=16`、`CONSOLE_LINES=8`、`CONSOLE_LINE_MAX=64`、`CONSOLE_WRITE_MAX=64`。
用户内核栈顶 A `0x201000`、B `0x202000`、C `0x203000`、D `0x204000`、E `0x205000`、F `0x206000`。
物理池仍为 `0x100000..0x1FEFFF` 的 255 页位图池。

### 异常与信号的状态机

1. 用户指令触发 `#DE/#BP/#UD/#GP`：CPU 压 error code（`#DE/#BP/#UD` 没有，由 stub 补 0），`exception_dispatch()` 记录 `vector/eip/err/cr2/comm`。
2. 查 handler：`process_signal_deliver()` 读当前进程 `sigactions[sig]`。没有 handler 走第 3 步默认终止；有用户 handler 时在用户栈写 signal frame，把 `irq_frame` 改成 `eip=handler`、`user_esp=frame_addr`，`exception_common` 直接 `iret` 进 handler。
3. 默认动作：`process_terminate_current(sig)` 把当前进程置 ZOMBIE，`exit_code = PROC_WAIT_STATUS(sig)`，`term_sig = sig`，自身 pending 置位，父进程置 SIGCHLD；随后 `task_exit()`。
4. `sigreturn`：handler 执行 `ret` 弹出 frame 第一个槽的 trampoline 地址，trampoline 执行 `eax=12 int 0x80`；`process_sigreturn_current()` 从 `frame->user_esp` 读回保存的 GPR/iret 帧，回到除零后的指令。
5. 父进程 `wait`：找到 ZOMBIE 后把 `exit_code` copyout 到用户 status，回收任务/AS/PID/栈，清父进程 SIGCHLD pending。
6. 回收：P10 前 `process_reap()` 保留 DEAD 墓碑；P10 后先把 `pid/ppid/exit_code/term_sig/image_id/argc/envc/comm` 写进 exit ring，再 `proc_zero()`，PCB 槽立即变 FREE。V 页改读 ring 的最新退出摘要，不再依赖墓碑。

## 二、P3.1 验收记录

QEMU 实测（本构建 `kernel.bin` 42,792 字节，BSS 结束 `0x8C140`，96 扇区 / 49,152 字节窗口）：

- `V` 页 PCB 表：`userA` PID 4 / AS 1 / READY；`userB` PID 5 / AS 2 / READY；`userC` PID 6 / `FAULT` / `SIG11`；`userD` PID 7 / AS 4 / READY；`userF` PID 8 / `GUARD` / `SIG11`；`userE` PID 9 / PPID 7 / `SIG` / `SIG8`。
- VMA 摘要：A/B 的 code RX `0x400000`、data RW `0x401000`、guard `0x402000`、stack RW `0x403000`、control R `0x404000`、heap RW `0x405000`，`brk 0x406000`。
- A/B 的 heap 验收：`brk 0x406000`、`heap poke 1`、`peek 0x48454C50`、`guard poke 0`；越界 `brk(0x500000)` 返回 `0xFFFFFFFF`。
- D 第一轮：`fork 9`、`wait 9`、`status 7`，页面 `WAIT OK 7 / COW OK`；D 写 `PAPA`，E1 写 `CHLD` 后 `exit(7)`，父读回 `PAPA` 后写 `COW`。
- D 第二轮：`fork2 9`、`wait9`、`st 80000008`、`SIG OK`；E2 先写 `SIGF` 到 COW 数据页，再执行 `1/0` 触发 `#DE`，父进程拿到 `0x80000008`。
- 信号面板：`#DE 1 pid 9 eip 00400206 sig 8 SIGFPE | SIGCHLD 2 pend 00000000`。`SIGCHLD 2` 来自 E1 与 E2 两次子进程退出；父进程两次 wait 都消费了 pending，因此当前 `pend 0`。
- `#PF count 3`：面板显示 `pid6 cr2 000B8000 err 00000007 count 3`；C 与 F 都被 SIGSEGV 终止，A/B 继续计数。COW 行 `forks 2 shared 8 live 2 breaks 4` 中的 `breaks` 包含内核 `copyout` 的拆分，不能把 `breaks 4` 直接当作 4 次 COW `#PF`。
- COW 行：第一轮 `forks 2 shared 8 live 2 breaks 4 last AS4 old 00115000 -> 00118000`；第二次启动 V 后累计为 `forks 4 shared 16 live 2 breaks 9 last AS11 old 00119000 -> 0011B000`。
- `PHYS PAGES used/total`：两轮都为 `20/255`；`int 0x80 traps` 第一轮 1,080,064，第二轮 2,208,997；`last CS 0000001B`。
- 再按 `V` 停止后回机械臂，机械臂 q 值正常；再按 `V` 重新出现 A/B/C/D/F/E，ASID 从 8 起、PID 仍复用 4-9，`PHYS PAGES` 仍为 `20/255`，没有 ZOMBIE/PID/页泄漏。
- `K` 页：独立启动后直接按 `K`，显示 `fault count 1`、`EIP 000837C1`、`CR2 00400000`、`ERR 00000002`、`CR3 00010000`、`kernel image 0x00080000..0x0008E200`；按 `R` 回主画面，机械臂继续更新。
- 若先跑完整 V 再按 `K`，K 页显示 `fault count 6`：V 页自身记录 3 次，V 停止路径额外产生 2 次用户 `#PF`（尚未逐条归因），再加 K 探针 1 次。K 页的 count 是累计值，不是单次 probe 次数。

## 三、P3.2 用户 handler 与 signal frame

### 3.1 最小动作表

- `struct process` 增加 `sigactions[PROC_NSIG]`，每项保存 `handler/mask/flags`；`handler=0` 是默认终止，`handler=1` 是忽略，其他值必须是当前地址空间内可读的用户 VA。
- `sigaction(sig,handler,mask)` 只接受 `1..31`；mask 存低 31 位，当前只随 frame 保存/恢复，不做阻塞。
- `fork` 复制父进程的 `sigactions` 与 `sig_mask`，不复制 pending；子进程 `sig_handled` 从 0 开始。
- 同步异常分发先查 handler；没有 handler、handler 非法、frame 写入失败时才退回 P3.1 的默认终止。

### 3.2 signal frame 布局

`frame_addr = (frame->user_esp - 64) & ~3`，必须落在 stack VMA `0x403000..0x404000` 且可写。低地址到高地址：

| 偏移 | 内容 |
| --- | --- |
| `+0` | `sigreturn_trampoline` 的用户 VA（handler 的 `ret` 目标） |
| `+4` | `eax,ebx,ecx,edx,esi,edi,ebp` |
| `+32` | `eip,cs,eflags,user_esp,ss` |
| `+52` | `sig,old_mask,magic(0x53494746)` |

投递时 `frame->eax=sig`、`frame->ebx=frame_addr+4`、`frame->eip=handler`、`frame->user_esp=frame_addr`；`exception_common` 的 `RESTORE_CTX` 弹出 GPR 后 `iret` 进 handler。

### 3.3 `#DE` 返回点与 `sigreturn`

- CPU 对 divide error 保存的 EIP 指向 faulting `div/idiv` 本身。`process_signal_deliver()` 对 `#DE` 做最小 `F6/F7` 解码（opcode + ModRM + SIB/位移），把保存的 `eip` 前移到下一条指令；解码失败则退回默认终止。
- `sigreturn` 的入口是 frame 第一个槽：handler 用 `ret` 进入 `user_sigreturn_trampoline`，执行 `eax=12 int 0x80`。此时 `irq_frame.user_esp` 指向保存的 GPR 区，`process_sigreturn_current()` 读回 60 字节 frame，校验 magic、CS/SS RPL3、恢复 EIP 与栈范围，再写回 GPR/iret 字段。
- `sigreturn` 恢复 `old_mask`；handler 可以通过 `ebx` 指向的 frame 修改保存的 EIP（`sigreturn` 以 frame 内容为准）。

### 3.4 系统调用与构建

- `eax=11`：`sigaction(sig,handler,mask)`，参数 `ebx/ecx/edx`；成功返回 0，失败 `0xFFFFFFFF`。
- `eax=12`：`sigreturn()`；成功用恢复后的 frame 返回，失败 `0xFFFFFFFF`。
- `loader.asm` 不需要新增 stub：`exception_common` 与 `int80_stub` 都已把 C 返回的 `struct irq_frame *` 写回 `esp`，`RESTORE_CTX` 直接消费改写后的帧。

### 3.5 第二次 fork 的 COW 继承修复

`address_space_clone()` 原来只保留 `PAGE_PRESENT|PAGE_USER`，对“已经是 COW”的页第二次 fork 会丢掉 `PAGE_COW`，把可写页退化成只读页。P3.2 改为：

- `share_writable = (pte & (PAGE_RW|PAGE_COW)) != 0 || control`；
- `flags = pte & (PAGE_PRESENT|PAGE_USER|PAGE_COW)`，共享时再补 `PAGE_COW`。

这样 D 第一轮 fork 后仍处 COW 的栈页，在第二轮 fork 后仍是 COW；内核写 signal frame 时能正常拆页，handler 也能使用该栈。

### 3.6 fork 子任务起跑与 lane 创建让出

第二次 fork 暴露出一个调度平局：子任务 `task_create_common()` 继承父任务的 `vruntime`，父进程进入 `wait` 轮询后，调度器会一再选回父任务，子任务永远轮不到，D lane 停在 `fork2` 之后。修复是在 `task_fork_user()` 里把新子任务的 `vruntime` 置 0，保证它至少先跑一次、完成自己的退出路径。

`user_demo_start()` 连续创建 5 条 lane 时也会长时间占住 render 任务。每建完一条 lane 调一次 `task_yield()`，让新 READY 的用户任务先获得一次调度机会，再继续创建下一条；失败按 `task_yield` 错误走统一失败屏。

## 四、P3.2 验收记录

> 注：以下为 P3.2 历史构建记录（当时 sig 轮用 `exit(9)`）；当前 sig 轮已加入 EFLAGS 白名单验收，改为 `in al,0x60` -> `#GP -> SIGSEGV(11)`，父 wait 得到 `0x8000000B`。

QEMU 实测（本构建 `kernel.bin` 44,392 字节，BSS 结束 `0x8D3C0`，96 扇区 / 49,152 字节窗口）：

- `V` 页 PCB：`userD` PID 7 / AS 4 / READY；`userE` PID 8 / PPID 7 / EXIT 9 / `userE`；handler 子进程没有再变成 `SIG8` 墓碑。
- `D fork2 8 wait 8 st 00000009 HAND OK`；`sig_ok` 是 `"HAND"`，不是 P3.1 的 `"SIG!"`。
- 信号面板：`#DE 1 pid 8 eip 0040021B sig 8 SIGFPE hdl 1 chld 2 pend 00000000`；`hdl 1` 证明 `SIGFPE` handler 被调用一次。
- `eip 0040021B` 是 faulting `div ecx` 的地址；返回后子进程执行 `0x40021D` 的 `cmp [0x401054],1`，确认 handler 计数为 1，再 `exit(9)`。
- COW 行 `forks 2 shared 8 live 2 breaks 5 last 4 old 00118000 -> 0011B000`；多出的 `breaks` 包含 signal frame 写入用户栈时的 COW 拆分。
- `PHYS PAGES used/total` 为 `26/255`，没有因为 signal frame 或 handler 泄漏页；`int 0x80 traps` 和 `last CS 0000001B` 正常。
- 再按 `V` 停止后回机械臂；没有 ZOMBIE/PID/页泄漏。

## 五、P4.2 用户态 exec 与 argv/env

### 语义

`exec` 只替换当前进程的用户地址空间和用户镜像，PID、PPID、`task_struct` 槽、内核栈、调度权重和父进程关系都不变。它不创建新进程，也不走 `fork` 的复制路径。P4.2 在 P4.1 的 AS 替换之上，增加了参数从旧用户空间到新用户栈的迁移。

- 入口：`process_exec_current(struct irq_frame *frame, uint32_t image_id, uint32_t argv, uint32_t envp)`，系统调用为 `eax=13`、`ebx=image_id`、`ecx=argv`、`edx=envp`。P4.2 只接受 `image_id==1`；其他值返回 `0xFFFFFFFF`。
- 前置校验：当前必须是 CPL3、`PROC_LIVE`、持有非空 `space`，且 `current_task` 与 `self` 一致。
- 参数解析：在旧 AS 仍有效时调用 `proc_exec_parse(self->space, argv, envp, &args)`。`argv` 和 `envp` 都是用户态 `char *` 数组，必须以 NULL 结尾；最多 `PROC_EXEC_MAX_VEC=4` 项，每串最多 `PROC_EXEC_STRING_MAX=32` 字节（31 字符 + NUL）。指针或字符串不可读、缺少 NULL、超过 4 项或超过长度上限都直接返回 -1，不分配新 AS。
- 新 AS：`address_space_create_user(user_exec_start, user_exec_end - user_exec_start)` 从 255 页池分配 PD/PT/code/data/stack/control；失败时旧 AS、旧帧和 `image_id` 都不动，返回 -1。
- 新栈：`proc_exec_build_stack()` 先算 `string_bytes = Σ(strlen+1)`，再按 `strings_base=(0x404000-string_bytes)&~3`、`envp_addr=strings_base-env_words`、`argv_addr=envp_addr-argv_words`、`esp=argv_addr-4` 布局，顺序为 `argc, argv[], NULL, envp[], NULL, 字符串区`。2 项参数 / 2 项环境时 `strings_base=0x403FE4`、`envp_addr=0x403FD8`、`argv_addr=0x403FCC`、`esp=0x403FC8`。字符串和指针数组都通过 `address_space_copyout()` 写入；超出一页或越界返回 -1，并释放新 AS。
- 切换：`cli` 下 `address_space_switch(new_space)` 写 CR3，`task_set_space(current_task, new_space)` 更新任务持有的 AS 指针，`self->space = new_space`；旧 AS 在所有引用切走后 `address_space_release()`。
- 信号状态：`proc_reset_signals(self)` 清空 `sigactions[32]`、`sig_mask`、`pending` 和 `sig_handled`。exec 后 handler 表不继承。
- iret 帧：`eax/ebx/ecx/edx/esi/edi/ebp=0`，`eip=0x400000`、`cs=0x1B`、`eflags=0x202`、`user_esp=new_esp`、`ss=0x23`；`fs/ds/es` 保持用户数据段。
- 元数据：`image_id=1`、`exec_argc=argc`、`exec_envc=envc` 写入 PCB；`fork` 让子进程继承三字段；P10 前 `reap` 保留在墓碑，P10 后写入 exit ring。`process_image_id(pid)` 和 `process_exec_counts(pid)` 先查活 PCB，再回退同 PID 的最新 ring，避免 PID 复用后旧记录覆盖新镜像。
- 失败原子性：解析、AS 分配、栈构造任一步失败都只释放新 AS，旧 AS、旧帧、旧 `image_id` 和旧参数计数全部保持；成功后才更新 PCB 和帧。

### 镜像

`user/exec/exec.asm` 是第二个固定链接在 `0x400000` 的 flat 镜像，由 `Makefile` 用 `nasm -f elf32` 汇编成 `build/user/exec.o` 并链进内核；运行时不读盘。镜像开头：

1. `eax=5 getpid`，把 PID 写到 `0x401000`。
2. 把 `0x45584543`（`"EXEC"`）写到 `0x401004`。
3. 把当前 `esp` 写到 `0x401008`，证明入口栈已重置。
4. 从 `[esp]` 读 `argc` 写到 `0x40100C`，从 `[esp+16]` 读 `envc` 写到 `0x401014`。
5. 逐字节校验 `argv[0]=alpha`、`argv[1]=beta`、`argv[2]=NULL`；再校验 `envp[0]=MODE=p42`、`envp[1]=LEVEL=2`、`envp[2]=NULL`。校验用寄存器先加载再比较，保留 `argc` 在 `eax` 供后续 `lea esi,[esp+eax*4+8]` 计算 envp 起点。
6. 全部匹配才把 `0x41525647`（`"ARGV"`）写到 `0x401018`、`0x454E5650`（`"ENVP"`）写到 `0x40101C`，`eax=2 yield` 300 次给 V 页观察窗口，最后 `eax=3 exit(10)`。
7. 任一校验失败写 `0x424144`（`"BAD"`）到两个验收字，并以唯一失败码退出：20 argc、21 argv0、22 argv1、23 envc、24 env0、25 env1。QEMU 的 wait status 可直接定位失败块。

D 第三轮由父进程 `fork` 出子进程；子进程准备静态数组 `exec_argv = {0x400312("alpha"), 0x400318("beta"), 0}`、`exec_envp = {0x40031D("MODE=p42"), 0x400326("LEVEL=2"), 0}`，并按镜像基址重定位成 `0x4002FA/0x400306` 后调用 `eax=13, ebx=1, ecx=argv, edx=envp`。父进程把 child pid、wait status、wait 返回值和 `EXEC/BAD` 验收字写到 D 页。

### 验收

QEMU 实测（P4.2 clean 构建 `kernel.bin` 47,304 字节，BSS `0x8B8E0..0x8E0E0`，96 扇区 / 49,152 字节窗口）：

> 注：本节为 P4.2 历史构建记录，sig 轮当时仍是 `exit(9)`。当前 sig 轮已改为 `in al,0x60` -> `#GP -> SIGSEGV(11)`，父 wait 得到 `0x8000000B`。

- P4.2 历史构建中 `D fork2 8 wait 8 st 00000009 HAND OK` 仍成立，说明 exec 没有破坏前两轮状态。
- `P4 exec 8 wait 8 st 0000000A img 1 EXEC OK arg 2 env 2`：子进程 PID 8 不变；exec 镜像 `image_id=1`；退出码 10 被父进程 `wait` 回收；`arg/env` 计数来自 `process_exec_counts(pid)`。
- 内核调试构建的串口日志曾输出 `P42 argc=2 env=2 a0=alpha e0=MODE=p42` 与 `P42S p0=alpha p1=beta e0=MODE=p42`，证明解析结果和新栈指针指向的字符串一致；该临时输出已在最终构建前移除。
- PCB `userE PID 8 PPID 7 EXIT 10`：wait 回收后 AS 已释放；P4.2 当时 `image_id=1` 保留在墓碑，P10 后改为 exit ring 摘要。
- 信号面板 `#DE 1 pid 8 eip 004002A6 sig 8 SIGFPE hdl 1 chld 3 pend 00000000`：第三轮 fork/exec 后 P3.2 的 handler 计数仍可见。`exception_info.handled` 在投递成功时快照，避免 PID 8 旧进程槽被清掉后按 PID 重查变成 0。
- COW `forks 3 shared 12 live 2 breaks 6 last AS4 old 0011B000 -> 0011C000`；`PHYS PAGES 20/255`；`int 0x80 traps` 持续增长，`last CS 0000001B`。
- 再按 `V` 停止后回机械臂；没有 ZOMBIE/PID/页泄漏。

## 六、P4.3 每进程 fd 与匿名 pipe

### 背景：P4.2 exec 参数（已完成）

- `exec(image_id, argv, envp)` 已把字符串复制到新用户栈，按 SysV 布局写 `argc/argv/envp`；P4.2 的真实 QEMU 验收为 `st 0000000A img 1 EXEC OK arg 2 env 2`。
- PID、AS 替换、信号重置和旧 AS 失败保护语义保持不变。

### 语义

- **fd 归属**：`kernel/proc/fd/fd.c` 是每进程 fd 表的唯一 owner。PCB 内嵌 `struct proc_fd_table fds`，`PROC_FD_MAX=8`；fd 0/1 明确预留给后续 console，`PROC_FD_FIRST=2`，匿名 pipe 从 2 起分配。每槽只保存 `pipe *` 和 `kind`（`CLOSED/PIPE_READ/PIPE_WRITE`）。
- **fork**：`process_fork()` 调 `proc_fd_table_copy()` 逐槽复制，并对每个非空端点 `pipe_acquire(p,write_end)`；复制失败时释放已建立的 pipe 引用、AS、栈和 PID，不留下半张表。父子两端都关闭后对象才回池。
- **exec**：`process_exec_current()` 只替换 AS、镜像、入口栈和信号状态，不调用 `proc_fd_table_close_all()`，也不改 fd 表。因此 exec 前后 fd 编号和 pipe 端点保持不变。
- **exit/terminate/reap**：`process_exit_current()`、`process_terminate_current()` 和 `process_reap()` 都先 `proc_fd_table_close_all()`；`proc_zero()` 也先关闭再初始化。这样正常退出、信号终止、父进程回收和 PCB 槽复用都不会漏掉 `pipe_release()`。
- **pipe 对象**：`kernel/ipc/pipe/pipe.c` 固定 `PIPE_MAX=4` 个对象、每对象 `PIPE_CAPACITY=64` 字节环形缓冲；维护 `head/tail/count` 和 `refs/readers/writers`。`pipe_create()` 只从 `used==0` 的槽取对象；`pipe_acquire()` 加总引用和端点计数；`pipe_release()` 减端点与总引用，`refs==0` 时清空并立即回池。
- **非阻塞 I/O**：`pipe_read()` 在缓冲为空且仍有写端时返回 `-1`，写端全关时返回 `0`；`pipe_write()` 在无读端或剩余容量不足时返回 `-1`；成功都返回字节数。实现没有等待队列，也不睡眠。
- **系统调用**：`eax=14 pipe(ebx=用户 u32[2])` 选两个空槽，写回读/写 fd；`eax=15 read(ebx=fd,ecx=用户缓冲,edx=长度)`；`eax=16 write(...)`；`eax=17 close(ebx=fd)`。失败统一 `-1`。用户范围校验和 `copyin/copyout` 都在 fd 模块内完成：read 最多 64 字节，write 超过 64 字节直接拒绝，坏指针不进入 pipe。
- **镜像 2**：`user/exec-pipe/exec_pipe.asm` 固定链接 `0x400000`，由 `Makefile` 汇编成 `build/user/exec_pipe.o` 并链进内核。它从 `argv={"pipe","writer","FD=3"}` 解析 fd，校验 `argc=3`、三个字符串，然后向该 fd 写 4 字节 `PIPE`，成功 `exit(12)`；失败码 26 argc、27 argv0、28 argv1、29 argv2、30 fd、31 write。
- **D 第四轮**：父 `pipe()` 得到 fd 2 读 / fd 3 写；`fork` 后父关闭 fd 3，子关闭继承的 fd 2 后 `exec(2)`。exec 不触碰 fd 表，新镜像继续持有 fd 3；它写 `PIPE` 后 `exit(12)`。父 `wait` 得到 12，从 fd 2 读 4 字节并校验 `0x45505049`，再关闭 fd 2；V 页写 `P4.3 pipe 8 wait 8 st 0000000C fd 2>3 r4 PIPE OK`，两端释放后 `pipe obj 0`。
- **双 exec 证据的确定性**：P4.2 与 P4.3 的子进程都复用 PID 8，不能用“当前最大 `image_id`”稳定区分两行。P4.2 当时在 PCB 里加 `PROC_REAP_HISTORY=2` 的 `reaped[2]`，由 `process_wait()` 回收前保存；P10 改为 `process_reap()` 写全局 exit ring，`process_reaped_exec(parent_pid,slot,...)` 按 `ppid` 取该父进程最近两次退出，slot 1 最新、slot 0 次新。固定 4 槽下，D lane 后续 spawn/thread 的 wait 会把 P4.2/P4.3 挤出，因此现在不再保证这两行固定可见。

### 验收

QEMU 实测（P4.3 final clean 构建 `kernel.bin` 50,760 字节，BSS `0x8C660..0x8F2E0`，104 扇区 / 53,248 字节窗口，`AEVUM_KERNEL_END=0x8F400`）：

- 同一帧同时出现 `P4 exec 8 wait 8 st 0000000A img 1 EXEC OK arg 2 env 2` 与 `P4.3 pipe 8 wait 8 st 0000000C fd 2>3 r4 PIPE OK`，说明 P4.2 的参数证据和 P4.3 的 fd 证据互不覆盖。
- `fd 2>3` 是父进程分配的真实槽位；`r4` 是父从 fd 2 读回的 4 字节；`PIPE OK` 表示 payload 校验为 `0x45505049`。执行顺序为 pipe -> fork -> 父 close 写端 -> 子 close 读端 -> 子 exec(2) -> 子写 `PIPE` -> 子 exit(12) -> 父 wait 12 -> 父 read 4 -> 父 close 读端。
- `pipe obj 0`：父子端点全部关闭后对象回池；`PHYS PAGES 20/255` 与停止前一致，没有 ZOMBIE/PID/页/fd/pipe 泄漏。
- `int 0x80 traps` 持续增长，`last CS 0000001B`，说明 pipe/read/write/close 都从真实 CPL3 系统调用门进入。
- 再按 `V` 停止后回机械臂，再按 `V` 重建：仍是 `20/255`、`pipe obj 0`、P4.2/P4.3 两行一致；V 停止→重启→再跑一轮也保持同样结果，证明 fd/pipe 引用、AS、PID 和进程历史都随新一轮清场。
- 取证说明：`screendump` 在 `-display none` / 无持续 VNC 客户端时可能读到旧显示 surface；验收以 QEMU monitor `pmemsave 0xB8000 4000 "/tmp/vga.bin"` 读到的 guest VGA 文本缓冲为准，再用等宽字体渲染成截图。稳定文本为 `PHYS PAGES used/total 20 /255`、两行 P4/P4.3 证据、`pipe obj 0`。

## 七、P4.4 console fd 0/1

### 语义

- **设备归属**：`drivers/console/console.c` 是 console 字符设备的唯一 owner；它只维护输入队列、输出行缓存、COM1 镜像和统计，不直接抢 VGA 布局。X 页渲染器只读 `console_line()`，VGA 写者仍是 `vga.c`。
- **fd 0**：`proc_fd_console_open()` 在新用户进程创建成功后安装 `CONSOLE_READ`；`read(0)` 从 16 字节 US ASCII 环形队列取数据，非阻塞，空队列返回 `-1`，有数据返回 1..len。输入源仍是 render 的 `keyboard_poll()`：X 页把非 `X` 的 scancode set 1 交给 `console_input_push()`，支持左右 Shift，release/未知键忽略。
- **fd 1**：同一入口安装 `CONSOLE_WRITE`；`write(1)` 最多 `CONSOLE_WRITE_MAX=64` 字节，追加到 8×64 行缓存，并逐字节 `uart_write()` 镜像 COM1。原始 ASCII 位于 VGA 差量帧之间，桌面 bridge 按 `A5 5A` 重新同步。
- **fork/exec/close**：`fork` 复制整张 fd 表；pipe 端点 `pipe_acquire()`，console 端点直接继承；`exec` 保留 fd 表；`close`、exit、terminate、reap 都只清本进程句柄。`proc_fd_entry_close()` 只对 pipe kind 调 `pipe_release()`，console 没有 pipe 指针，不能走 pipe 释放路径。
- **共享边界**：console 是全局字符设备，没有进程私有副本，也没有前台焦点；所有持 fd0 的进程竞争同一条输入队列，所有持 fd1 的进程写同一行缓存和 COM1。`close` 只关闭调用者的句柄；只要另一个进程仍持有端点，console 对象继续工作。
- **X 页演示**：mode 4 用户进程先 `fork`。父进程关闭自己的 fd0/fd1，验证 `read(0)=-1`、`write(1)=-1`，设置 `CON!` 后保持存活；子进程继续用继承端点循环读 fd0，读到按键后写 `CON:<key>` 和 `FD01 OK`，再 `exit(14)`。父进程不 `wait`，避免在 X 页里再引入一轮 `yield` 轮询；`wait` 语义已由 P4.3 的 D 第四轮覆盖。页面用 `out 14` 与 `child EXIT 14` 判定子进程完成。

### 验收

QEMU 实测（P4.4 final clean 构建 `kernel.bin` 55,208 字节，BSS `0x8D7C0..0x906D4`，112 扇区 / 57,344 字节窗口，`AEVUM_KERNEL_END=0x90800`）：

- X 页首屏显示 `AEVUM / CONSOLE FD 0/1`，`pid 4 child 5`，父进程关闭 0/1 后 `close0 0 close1 0 r0 -1 w1 -1`。
- 发送 `a` 后同一帧变为 `scan 3 push 1 frames 1590`、`in 1 last a q 0 drop 0`、`out 14 uart 14`、`child EXIT 14`、`CLOSE OK`；输出区两行 `CON:a` 与 `FD01 OK`。
- COM1 原始日志中包含 `CON:a\nFD01 OK\n`，同时仍有 `A5 5A` 差量帧，说明 fd1 文本与桌面 VGA 协议共用 COM1 时没有破坏帧同步。
- 按 `X` 退出后回到机械臂主画面，再按 `X` 可重新进入并再次读键；页面帧计数持续增长，子进程 `out` 和输出行重置后重新建立。
- 取证同样使用 `pmemsave 0xB8000 4000` 读取 guest VGA 文本缓冲，再渲染成 PNG；串口日志用原始字节检查。

## 八、P5.1 槽池与动态内核栈

P5.1 把容量从 P4.4 的 10/8/8 提到 `TASK_MAX=16`、`PROC_MAX=12`、`AEVUM_AS_MAX=12`，并把原来 6 个固定用户内核栈改成 12 个 4KB 槽的位图池 `0x200000..0x20BFFF`（槽 0 顶 `0x201000`，槽 11 顶 `0x20C000`）。`kernel/task/stack/stack.c` 唯一持有位图；`kernel_stack_alloc()` 在创建/fork/kthread 时取槽，`kernel_stack_free()` 在失败路径、`process_reap()` 和 `task_reap_kthread()` 归还，分配与释放都整槽清零，避免旧中断帧穿透到下一个进程。P8 起该池同时服务用户进程/线程和动态 kthread，静态 control/render/idle 仍用固定栈。

`Y` 页让父进程连续 fork 8 个子进程并全部保持 LIVE，页面同时显示 `task 12/16`、`proc 9/12`、`AS 9/12`、`kstack 9/12`、`live 9`、`root 4`、`children 8/8`、`POOL OK` 和 12 行槽表；PID 4..12 对应栈顶 `0x201000..0x209000`。按 `Y` 停止后父 `exit(16)`、子 `exit(17)`，procs 前 9 槽 PID 归零；再次按 `Y` 仍得到 9/12/12/12，并复用 PID 4..12 与同一批栈顶，证明槽位、地址空间和 4KB 栈都完成回收与复用。

P5.1 clean 构建 `kernel.bin=57,768`，SHA256 `16EC038ED0F9F0A236C44DDD370B62323D181AE1616D94D4DE3C78E34015042B`；`boot.bin=512`，`D9A70CE2062C369AA0F08BDF6E98906E6D38F75FF10878F22FB6988B39ADB6ED`；`disk.img=1,474,560`，`85CAC1864BCB80B378EC93966855348B5E7F748DCBA5BD36D342DDC2FEC49970`。`AEVUM_KERNEL_END` 当时为 `0x92000`。

## 九、P5.2 V 页分页与观测面

### 9.1 UI 状态与绘制分离

P5.2 新增 `include/app/user/page.h` 与 `app/user/page/page.c`，由 page 模块唯一持有 `page_mode`、`page_slot`、`page_first`；`USER_PAGE_COUNT=4`、`USER_PAGE_ROWS=10`。`app/arm/app/app.c` 在 `user_live` 时把扫描码交给 `user_page_key()`，不再在 arm app 内散落翻页逻辑。

键位：`Tab`（`0x0F`）循环切页，数字 `1..4`（`0x02..0x05`）直达 `SLOTS/TREE/ADDRESS/FD+SIG`，`Up/Down`（`0x48/0x50`）移选中槽，`PgUp/PgDn`（`0x49/0x51`）整窗滚 10 槽，`Home/End`（`0x47/0x4F`）跳首尾；`keyboard_poll()` 的 `0xE0` 前缀由 page 模块吞掉，下一帧的方向键实际码才生效。空槽详情仍显示 `EMPTY` 与池统计。

渲染拆成 `render.c`（公共标题、页签、slot/pid/page 状态、底栏与页面分派）、`draw.c/.h`（统一 `put_dec/put_hex`、进程状态名/颜色、VMA kind 名、fd kind 名）、`slots.c`（10 行窗口覆盖 12 槽，选中槽含 KSTACK/CR3/BRK/VMA、image/argc/envc/switches/ticks、信号、children/WAITER、池统计）、`tree.c`（按 PPID 递归，根 `R0`、子节点 `+depth`，`CH` 子进程数、`W` ZOMBIE 子进程数，visited 防环）、`address.c`（6 条 VMA 的 START/END/FLAGS 与真实 PTE R/W 探针，COW/PHYS、信号、exec/reap 历史、REAP0/1、COW last、children/WAITER，底部 P3/P4 验收）、`fd.c`（8 个 fd 槽逐项 kind/open、fd 打开数、pipe 端点数、信号、最近异常、最近 `#PF`、pipe 对象数、children/WAITER）。

### 9.2 只读观测 API

- `include/kernel/proc/fd.h` 新增 `struct proc_fd_info { int used; u32 kind; }` 与 `proc_fd_snapshot(...)`；`kernel/proc/fd/fd.c` 逐 fd 返回 kind，不暴露 pipe 指针。
- `include/kernel/proc.h` 新增 `struct proc_fd_info` 前向声明、`process_slot_space(int slot)`、`process_slot_fd_info(int slot,int fd,...)`；`kernel/proc/process.c` 按 PCB 槽直接取 AS/fd，DEAD/FREE 槽返回空。
- 保留 `process_slot_used()` 原语义（只数 LIVE/ZOMBIE）；`V` 页的 `POOL proc` 显式统计 `live+zomb+dead`。P10 后回收槽直接 FREE，`dead` 固定为 0。

### 9.3 ZOMBIE 观察窗口

为让 ZOMBIE 在 `V` 页稳定可观察，`user/ring3/ring3.asm` 在 D lane 父进程 `wait` 前增加基于 `0x4010D0` 的 yield 间隔，最终值 262144。子进程 `userE` 退出后保持 ZOMBIE 数秒，父 `userD` 随后仍执行原 wait/COW 验收；这个改动只影响 D lane 时序，不改变 wait 结果。

### 9.4 QEMU 验收

clean 构建 `kernel.bin=64,872`、`boot.bin=512`、`disk.img=1,474,560`；kernel SHA256 `48B046EB73CB31C2378E1FBBC90C01C8812F8E62AF46883B4DABA427285664D8`，boot `D9A70CE2062C369AA0F08BDF6E98906E6D38F75FF10878F22FB6988B39ADB6ED`，disk `97379832264407B170E94BAE97F4422623E1CD34987DB23C0E85E086486E4E7C`。增量构建与 `make clean && make all` 的 kernel 哈希一致；`__bss_end=0x939F8`，`AEVUM_KERNEL_END=0x94000`。

- page 1 SLOTS：`slot 0 PID 4`、`window 1-10/12`、`SLOT 4 PID 8 PP 7 ST ZOMB TK ZOMB EX 7 ASID 5`、`POOL proc 6/12 live 3 zomb 1 dead 2 task 7/16 AS 4/12`。
- page 2 TREE：`roots 5 used 6/12 live 3 zomb 1 dead 2`；`PID 7 userD CH 1 W 1`，其下 `+1 PID 8 PP 7 ST ZOMB`。
- page 3 ADDRESS：`userA CR3 00100000 ASID 1 VMA 6`，6 条 VMA `00400000..00406000`，CODE `R-X PTE R-`、DATA/STACK/HEAP `RW- PTE RW`、GUARD `--- PTE --`、CTRL `R-- PTE R-`；`TEST wait OK cow OK hand OK exec OK pipe OK`。
- page 4 FD/SIG：fd0 `CONR kind 3`、fd1 `CONW kind 4`、fd2..7 closed，`OPEN fd 2/8`；`EXC #DE cnt 1 pid 8 eip 00400606 sig 8 SIGFPE hdl 1`；`#PF pid 7 eip 004002A4 cr2 00401080 err 00000007 count 8`。
- 滚动：`PgDn` 后 `window 3-12/12`，`SLOT 2..11` 全部出现；再 `Up` 两次选中槽 4，详情为 `TASK ZOMB ... AS CR3 0011A000 ASID 5`，窗口不变。

## 十、P5.3 100 轮 fork/wait 压测

### 10.1 落点与归属

P5.3 不新增页面：`S`（`0x1F`）复用 P5.1 的池页外壳，`pool_demo_start(int stress)` 仍是 `poolA/stressA` 创建、模式写入和首次 yield 的唯一入口，`stress ? 6u : 5u` 选择 ring3 mode 6 或 P5.1 mode 5。`pool_demo_is_stress()` 只供 `app/arm/app/app.c` 选择退出键（`S`/`Y`），页面仍由 `app/pool/render/render.c` 唯一绘制。

数据布局复用 P5.1 用户页，不新增结构体：`0x4010C0` 已完成轮次，`0x4010C4` 完成魔数 `POOL`，`0x4010C8` 只执行一次标记，`0x401100/0x401104/0x401108` 首 PID/末 PID/wait status。`pool_demo_child_count()` 在 stress 下返回 0..100，且跳过 8 项 PID 缓存循环；`pool_demo_child_pid(0/1)` 读首/末 PID。

### 10.2 ring3 校验

父进程 100 轮：`eax=6 fork`；子进程 `eax=3, ebx=7 exit(7)`；父进程以本轮返回子 PID 调 `eax=4 wait`，校验返回值等于子 PID、status 等于 7；首轮 PID 写 `0x401100`，后续必须与首轮相同。任一失败立即停在 `<100` 且不写魔数；全部通过才写 `0x4010C4="POOL"` 并保持存活。`S` 停止时父进程 `exit(18)`，由现有 `process_wait/reap/cleanup` 路径回收 fd、task 槽、AS、PID、内核栈和物理页。

### 10.3 构建预算

新增代码后首次构建 66,120B 超限。最终把 `Makefile` 的 C 目标加 `-ffunction-sections -fdata-sections`、链接加 `--gc-sections`，回收约 1.3KB 不可达代码；clean `kernel.bin=64,808`，余量 728B，`__bss_end=0x9395C`，`AEVUM_KERNEL_END=0x94000`。

### 10.4 QEMU 验收

两轮都从 fresh boot 的机械臂主画面按 `S` 进入，每轮等 `round 100/100` 与 `STRESS OK` 后按 `S` 停止、回主画面再启动下一轮：

- 第一轮（P5.3 当时）：`round 100/100 first 5 -> 5 live 1 state STRESS OK`，`PHYS 6/255 proc 1/12 AS 1/12 KSTK 1/12`，进程表 `PID 4 LIVE stressA`、`PID 5 DEAD userE`；ASID 1。P10 回收即清零后，同一压测页面只剩 `PID 4 LIVE stressA`，不再有 `PID 5 DEAD` 行；复测仍为 `round 100/100 first 5 -> 5 live 1 state STRESS OK`。
- 第二轮（P5.3 当时）：`round 100/100 first 5 -> 5 live 1 state STRESS OK`，`PHYS 6/255 proc 1/12 AS 1/12 KSTK 1/12`；ASID 102，首末 PID 仍为 5。
- 两轮间 PID、AS 槽、内核栈槽与物理页计数完全一致；ASID 变化只说明地址空间槽被重新分配后再次复用，没有累积。

## 十一、后续阶段

### P5：规模与可观测性

- P5.1 已完成：16/12/12 任务/进程/AS 槽位，12 个 4KB 用户内核栈位图池，`Y` 页 9 个 LIVE 进程与两轮槽位/栈复用。
- P5.2 已完成：`V` 页 4 页分页/滚动，进程树、ZOMBIE、waiters、VMA、信号、fd 全部可观察；clean `kernel.bin=64,872`，距 65,536 上限余 664 字节。
- P5.3 已完成：`S` 复用池页执行 100 轮 fork/wait，首末 PID 均为 5，两轮 `PHYS 6/255`、`proc/AS/KSTK 1/12`；clean `kernel.bin=64,808`，余量 728B。
- P6.0 已完成细节收口：`process_wait()` 对 `pid>0` 先校验直接子进程，非子进程或不存在立即返回 -1；`pid<=0` 等任意直接子进程。`proc_orphan_children()` 同时处理 LIVE 和 ZOMBIE 直接子进程，并记录最近一次 `from/to/children[]` 事件；V 页 TREE 增加 `ORPHAN` 两行，G 页 reparent 面板允许显示 4 个子进程并在超过 4 个时显示 `+N more`。clean `kernel.bin=64,488`，余量 1,048B。当前速度基线见 `PERFORMANCE.md`。
- P7.1 已完成最小线程组：D lane 的 worker TID 6 与进程 PID/主线程 TID 7 同组，共享计数器为 1，join 返回 0、status 0；V 页显示 `THREAD tid 6 join 0 shared 1 THRD` 和 `thr OK`。clean `kernel.bin=70,568`，上限 98,304B。
- P7.2 已完成线程 detach：第二个 worker 自 `thread_detach` 后退出，主线程 join 返回 -1；调度器延后回收 detached 任务槽/内核栈，页面显示 `det OK`。clean `kernel.bin=71,272`，`__bss_end=0x955BC`。
- P7.3 已完成主线程先退与参数矩阵：子进程主线程先 `thread_exit(0)`，worker 继续运行并 join 主 TID，确认 status 0 后作为最后线程 `exit(7)`；父进程 wait 得到 7。join/detach 的 0、-1、self、不存在 TID 全部返回 -1，页面显示 `main OK arg OK`。clean `kernel.bin=72,040`，`__bss_end=0x9587C`。
- P7.4 已完成主线程不 join 的整组回收：子进程主线程创建 A/B 两个 worker 后只写退出标记并 `thread_exit(0)`，A 退出 5，B join A 得到 status 5 后作为最后线程 `exit(7)`；父 wait 得到 7，`process_reap()` 回收整组任务槽。页面显示 `grp OK`。clean `kernel.bin=72,744`，`__bss_end=0x95B3C`。
- P7.5 已完成阻塞 wait/join：父在子进程仍 LIVE 时 `wait` 进入 BLOCKED；子主线程 `thread_join` 先于 worker 退出阻塞；worker 退出 4、子写 `BLK5` 后退出 9；父被唤醒得到 status 9 并写 `WBLK`。页面显示 `blk OK`、`blk 7 /1`，`TEST` 全部 OK。clean `kernel.bin=73,992`，`__bss_start=0x92120`、`__bss_end=0x9601C`，`AEVUM_KERNEL_END=0x97000`。
- P8 已完成动态内核线程：control 启动时创建 `kping`，它拿到 TID 4 和栈池槽，yield 192 次后退出 5；control 在 join 上阻塞并被唤醒。页面显示 `kthr 4 s5 OK`，tasks dump 中 slot 3 与 TID 4 已归还，计数 `created=1 exited=1 reaped=1 join_blocks=1 join_wakes=1`。clean `kernel.bin=75,336`，`__bss_start=0x92660`、`__bss_end=0x9669C`，`AEVUM_KERNEL_END=0x97000`。
- P9 已完成未 join kthread 的 grace 兜底：`kping` 仍正常 join，另创建不 join 的 `korph`，它退出 6 后超过 100 tick 被调度器回收；页面 `kt 4 s5 a1 OK`，`reaped=2`、`auto_reaped=1`、`live=0`，动态栈水位回到创建前。clean `kernel.bin=77,160`，`__bss_start=0x92D80`、`__bss_end=0x96DFC`，`AEVUM_KERNEL_END=0x97000`。

### P7.1 最小线程组（Linux 风格资源共享）

目标不是一次补齐 pthread，而是先建立 Linux “线程 = 共享资源组中的任务” 的最小模型：

- `struct process` 继续作为资源组，`pid` 是 TGID；`struct task_struct` 是调度实体，`pid` 是 TID，新增 `tgid`。进程的父子关系、`wait`、SIGCHLD 仍按 TGID 工作，线程不进入子进程表。
- `clone` 只接受 `CLONE_VM|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD`，即共享当前地址空间、fd 表和信号 handler 表；不声称支持 `CLONE_FS`，因为当前没有 cwd/root/umask 状态。
- 每个线程独立持有任务槽、TID、4KB 内核栈、用户栈和 FPU/SSE 现场；`task_create_thread_user()` 复用统一中断帧和 `task_create_common()`，只把初始 `eax` 设为 `arg`，并让新线程 `vruntime=0` 以尽快获得首轮调度。
- `thread_exit` 只结束当前任务。`process_thread_exit_current()` 在活线程数仍大于 1 时把当前任务置 ZOMBIE；只有最后一个活线程退出时才调用 `process_exit_current()`，让整个进程进入 ZOMBIE 并通知父进程。
- `thread_join` 只接受同组、非当前、已经 ZOMBIE 的 TID；轮询 `task_yield_wait()` 等待，复制 `exit_code` 到用户 status，再 `task_reap_thread()` 释放该线程的内核栈和 TID。
- `process_reap()` 不再只释放 leader，而是 `task_release_process_threads()` 释放整组所有任务槽和内核栈；`process_alive()` 改为“还有活线程”；`process_snapshot()` 优先返回非 ZOMBIE 线程并暴露 `tid/thread_count`。
- 多线程进程的 `exec` 第一版直接拒绝，避免其他线程继续持有旧 AS 指针；这是明确的临时边界，不是完整 Linux exec 语义。

当前 D lane 验收顺序：主线程 `brk(0x406000)` 后把 `0x405000..0x405FFF` 作为 worker 栈，调用 `clone(user_thread_entry, 0x406000, 0x00010D00, &shared_counter)`。worker 从 `eax` 取参数，对共享计数器加 1，`gettid` 写入 D 页，写完成标记后 `thread_exit(0)`；主线程等完成标记后 `thread_join(tid, &status)`，校验 join 返回 0、status 0、共享计数器 1，写 `0x54485244`（`THRD`）。

QEMU 实测（P7.1 clean 构建 `kernel.bin=70,568`，`__bss_end=0x952BC`，`AEVUM_KERNEL_END=0x96000`）：

- V 页 ADDRESS：`THREAD tid 6 join 0 shared 1 THRD`，`TASK READY` 行 `THR 1 TID 7`；`TEST wait OK cow OK hand OK exec OK pipe OK spawn OK thr OK`。
- 线程组进程 PID 7，主线程 TID 7，worker TID 6；join 后线程任务槽和内核栈已回收，所以进程只剩 `THR 1`。
- 共享计数器为 1、join status 为 0、join 返回值为 0，说明 worker 改的是同一 data 页，而不是 clone 出独立副本。

已知边界：只验证了正常 clone/exit/join；主线程先退、worker 继续、线程重复 join、join 自己、线程触发 `#PF/#DE`、多线程 exec 拒绝返回值和 `process_stop_all()` 的线程组停止语义尚未单独压测。`thread_shared` 抓屏为 join 后状态，未截到线程运行中的 `THR 2`。

### P7.2 thread_detach（自分离线程）

P7.2 把线程生命周期从“只能 join”扩到“可 detach”：

- 每个任务新增 `detached` 标志；`eax=23 thread_detach(ebx=tid)` 只接受同组 TID，成功返回 0，失败返回 -1；允许线程 detach 自己，也允许主线程 detach 其他 worker。
- `thread_join` 读到 `detached=1` 立即返回 -1，不再等待或复制 status；detached 线程的退出码不再被 join 消费。
- detached 线程退出仍先进入 ZOMBIE；`task_reap_detached()` 在调度器下一次切换时回收所有非 current 的 detached 僵尸任务，释放任务槽、TID 和内核栈。当前退出线程的栈不能被自己释放，所以必须等下一次切换。
- 最后一个活线程退出时仍走 `process_exit_current()`；detached 不会改变“最后线程退出才结束进程”的规则，也不会绕过父进程 `wait` 对 TGID 的回收。
- `task_release_process_threads()` 仍是线程组整体回收的兜底路径；已经被 `task_reap_detached()` 清掉的槽不会再被二次释放。

D lane 新增第二个 worker：它从 `eax` 取得共享计数器地址，调用 `gettid` 后执行 `thread_detach(self)`，写计数器、保存 detach 返回值、置完成标记，再 `thread_exit(0)`。主线程等待完成标记后调用 `thread_join(detached_tid, 0)`，必须得到 -1；同时校验 detach 返回值 0、计数器 1，写 `0x44544544`（`DETD`）。

QEMU 实测（P7.2 clean 构建 `kernel.bin=71,272`，`__bss_end=0x955BC`，`AEVUM_KERNEL_END=0x96000`）：

- V 页 `THREAD tid 6 join 0 shared 1 THRD det OK`；`TEST ... thr OK det OK`；`TASK READY` 行 `THR 1 TID 7`。
- joined worker TID 6 已由 join 回收；detached worker 自 detach 后 join 返回 -1，任务槽和内核栈由调度器延后回收；页面仍为 `THR 1`，没有留下 detached 僵尸槽。

### P7.3 主线程先退与非法参数矩阵

P7.3 不改线程创建语义，只验证并压实已有生命周期边界：

- D lane 在 detach worker 之后 fork 一个子进程。子进程主线程先 `gettid`，再 `clone` worker，写 `0x40119C=1` 后直接 `thread_exit(0)`；此时活线程数为 2，`process_thread_exit_current()` 只把主任务置 ZOMBIE，进程保持 LIVE。
- worker 从 clone 的 `eax` 收到主 TID，先校验自己的 `gettid` 不等于主 TID；看到退出标记后调用 `thread_join(main_tid, &status)`。主线程已经 ZOMBIE，join 复制 status 0，并 `task_reap_thread()` 回收主任务槽与内核栈；worker 继续在共享 AS 上运行，写 `MAIN`。
- worker 作为最后活线程调用 `thread_exit(7)`，`task_process_live_threads(self)<=1` 才进入 `process_exit_current()`，进程转 ZOMBIE；父进程 `wait` 得到 7，写 `MTHR`。如果主线程退出时误结束整组，worker 不会运行，wait 只能得到 0。
- D 主线程在 fork 前校验 `thread_join(0/-1/self/99)` 与 `thread_detach(0/-1/99)` 全部返回 -1，写 `EDGE`；这些调用分别覆盖 `tid<=0`、`tid==current`、找不到同组任务三类拒绝路径。

QEMU 实测（P7.3 clean 构建 `kernel.bin=72,040`，`__bss_start=0x91980`、`__bss_end=0x9587C`，`AEVUM_KERNEL_END=0x96000`）：

- V 页 `THREAD tid 6 join 0 shared 1 ok THRD det OK main OK arg OK`；`TEST ... thr OK det OK`；`TASK READY` 行 `THR 1 TID 7`。
- P7.3 子进程父 wait 返回子 PID、status 7，说明主线程先退后 worker 确实继续执行并 join 了主线程；`EDGE` 与 `MTHR` 同时为 OK。

### P7.4 主线程不 join 与整组回收

P7.4 把线程组回收的最后一条路径补齐：主线程不等待 worker，也不被任何线程 join，最终由最后活线程触发进程退出，再由父进程 `wait` 一次回收整组。

- D lane 在 P7.3 成功后 fork 一个子进程。子进程主线程 `gettid`，然后 `clone` worker A 和 worker B；两个 worker 不等主线程 join，主线程只写 `0x4011D4` 退出标记并 `thread_exit(0)`。此时组内还有 3 个活线程，`process_thread_exit_current()` 只把主任务置 ZOMBIE，进程保持 LIVE。
- worker A 看到标记后直接 `thread_exit(5)`，不 join 任何人；此时组内仍有 worker B 和主 ZOMBIE 两个任务（主线程已是 ZOMBIE，不计活线程），worker B 成为最后活线程。
- worker B 看到标记后调用 `thread_join(A_tid, &status)`，校验 join 返回 0、status 5，写 `0x4011E4=GRP4`；随后调用 `thread_exit(7)`。因为 `task_process_live_threads(self)<=1`，它进入 `process_exit_current()`，进程转 ZOMBIE 并通知父进程。
- 父进程 `wait` 得到子 PID、status 7，写 `0x4011F8=MGRP`；随后 `process_reap()` 调 `task_release_process_threads()` 释放整组所有任务槽、TID 和内核栈，包括从未被 join 的主 ZOMBIE。页面 `grp OK` 读的是 `MGRP`。
- 等待轮询从 `task_yield()` 改为 `task_yield_wait()`：普通 yield 仍不记 vruntime，只有 `process_wait()` 与 `thread_join` 的轮询先记 `WEIGHT_QUANTUM / weight` 再切走。此前 `wait` 轮询不记账，D 以最低 vruntime 反复抢占，spawn 子进程被饿死；全量给所有 yield 记账又会把 D 的 26 万次 yield 计入 vruntime，反而使 D 长期得不到 CPU。P7.4 当时按“只记等待轮询”折中，QEMU 已验证 spawn 与 P7.4 都能推进；P7.5 已改为真正阻塞，普通 yield 和阻塞等待都不再需要为“空转公平”记账。
- `process_snapshot()` 在 `has_task` 分支改用 `task_info.stack_top`，避免主线程被 join 后展示可能过期的 `process->stack_top`。P7.4 的主线程没有被 join，但这一口径仍用于同一组任务快照。

QEMU 实测（P7.4 clean 构建 `kernel.bin=72,744`，`__bss_start=0x91C40`、`__bss_end=0x95B3C`，`AEVUM_KERNEL_END=0x96000`）：

- V 页 `THREAD tid 6 join 0 shared 1 ok THRD det OK main OK arg OK grp OK`；`TEST ... thr OK det OK`；`TASK READY` 行 `THR 1 TID 7`。
- 父 `wait` 返回子 PID、status 7；P7.4 子进程整组（主线程 + A + B）在 tasks 表里全部消失，只剩父 lane 的 PID 7 与常驻任务，证明未 join 的主 ZOMBIE 也被 `process_reap()` 回收。
- `GRP4` 与 `MGRP` 同时为 OK，说明 A 的 status 5 被 B 正确读取，B 以 7 退出并被父进程观察。

### P7.5 阻塞 wait/join 与 lost wake 防护

P7.5 把 P7.4 的“等待轮询”替换成真正的睡眠/唤醒。关键不是把 `task_yield_wait()` 改名，而是让“重查条件”和“进入 BLOCKED”成为不可分割的临界区：

- `task_block_until(ready, ctx)`：保存 flags 后 `cli`；调用 `ready(ctx)`。若条件已满足，恢复 flags 返回 0，不阻塞；否则把 `current_task->state` 写成 `TASK_BLOCKED`、`sched_reason=SCHED_BLOCK`，再 `int $0x40`。从 `cli` 到 `int 0x40` 之间没有 PIT，也没有其他任务能运行，所以不会出现“最后一次检查没有 ZOMBIE，刚准备睡，子进程退出并唤醒，父进程仍然睡死”的 lost wake。被唤醒返回后恢复 flags，返回 1；当前任务不是 RUNNING 或 `sched_failed` 返回 -1。
- `process_wait()` 使用 `proc_wait_ready()`：出现可回收 ZOMBIE 子进程、指定 PID 已不是直接子进程、或已经没有子进程时返回 1；否则阻塞。`process_thread_join_current()` 使用 `proc_join_ready()`：目标 TID 不存在、已 detached、或已 ZOMBIE 时返回 1。
- 唤醒点放在调度器而不是退出函数：`task_schedule_irq()` 在 `prev->state == TASK_ZOMBIE`、`fpu_save(prev->fpu_state)` 之后、`task_reap()` 之前调用 `process_notify_task_zombie(prev->proc)`。此时目标任务已经不能再运行，等待者被唤醒后重查一定能看到 ZOMBIE；如果在 `process_thread_exit_current()` 提前唤醒，目标任务还可能处于 RUNNING，等待者会再次阻塞，形成 lost wake。
- `process_notify_task_zombie()` 先 `task_wake_process(proc)` 唤醒同组所有 `TASK_BLOCKED` 任务，供 `thread_join` 使用；若 `proc->state == PROC_ZOMBIE` 且 `ppid>0`，再找父进程并唤醒父进程组，供 `wait` 使用。
- 全局 `wait_block_total / wait_wake_total / join_block_total / join_wake_total` 只统计 wait/join 的实际阻塞与唤醒，不含 idle 的 `task_block`；`process_block_stats()` 导出。V 页 `blk` 行显示 `wait_blocks / join_blocks`，`blk OK` 同时要求 `WBLK`、四个计数非零。

D lane 的 P7.5 轮次在 P7.4 成功后执行：

- 父 `fork` 子进程后立刻 `wait`，此时子进程仍 LIVE，父必须进入 BLOCKED。
- 子进程主线程 `gettid` 后 `clone` worker，立刻 `thread_join(worker)`；worker 尚未运行或仍在等 `0x401208=1`，join 必须进入 BLOCKED。
- worker 看到启动标记后执行 1024 次 `yield`，再 `thread_exit(4)`；子主线程 join 得到 status 4，写 `0x40121C=BLK5`，再 `thread_exit(9)`。
- 父 `wait` 被唤醒后得到子 PID、status 9，写 `0x40122C=WBLK`；页面 `blk OK` 读 `WBLK`，并检查 wait/join 的阻塞与唤醒计数。

QEMU 实测（P7.5 clean 构建 `kernel.bin=73,992`，`__bss_start=0x92120`、`__bss_end=0x9601C`，`AEVUM_KERNEL_END=0x97000`）：

- V 页 `THREAD tid 6 join 0 shared 1 ok THRD det OK main OK arg OK grp OK`；`TEST wait OK cow OK hand OK exec OK pipe OK spawn OK thr OK det OK blk OK`；`EXEC ... THR 1 TID 7 blk 7 /1`。
- `blk 7 /1` 表示 7 次 wait 阻塞和 1 次 join 阻塞；`wait_wakes` 与 `join_wakes` 均非零，否则 `blk OK` 不会成立。
- P7.5 子进程先以 status 9 成为 ZOMBIE，父随后被唤醒并 `process_reap()`；tasks 表只剩 control/render/idle/userA/userB/userD 六个任务槽，P7.5 子进程的主线程与 worker 全部消失。
- P7.4 的 `grp OK` 路径在 P7.5 构建下仍通过，说明阻塞改造没有破坏整组回收。

### P8 动态内核线程

P8 的“动态”不是换一个任务名字，而是把静态内核任务没有的三件事交给运行时：

- 任务槽和 TID 在 `kthread_create()` 时从 `TASK_MAX/PID_MAX` 池分配，不是启动时 `task_create()` 固定占位。
- 内核栈从 `kernel_stack_alloc()` 的 12 槽 4KB 池取得；静态 control/render/idle 继续使用 `AEVUM_STACK_P1/P2/IDLE` 固定栈，不进入这条路径。
- 退出后不立即消失：`kthread_exit(code)` 把当前任务置 ZOMBIE，`task_reap()` 明确跳过它；优先由另一个内核任务 `kthread_join(tid,&status)` 读到退出码后回收，无人 join 时由 P9 的 grace 兜底回收。

入口参数通过 `task_create_kernel_arg()` 的 `kernel_entry/kernel_arg` 字段保存，再由 `task_kernel_arg_entry()` trampoline 在 ring0 调用。没有给初始中断帧塞参数寄存器，也没有复用用户线程的 `eax` 约定。

join 复用 P7.5 的 lost wake 防线：

- joiner 先写 `kthread_wait_tid=tid`，再进 `task_block_until()`；`kthread_join_ready()` 在 `cli` 窗口内重查目标是否已 ZOMBIE。
- 调度器只在 `prev->state == TASK_ZOMBIE`、`prev->kthread` 为真时调用 `task_wake_kthread_waiters(prev->pid)`，按等待 TID 唤醒 joiner；提前唤醒会让等待者重查后再次睡死。
- `kthread_join()` 读出 `exit_code` 后调 `task_reap_kthread()`，先 `kernel_stack_free(stack_top)`，再 `pid_free`，最后 `task_zero`；`task_zero` 现在也清 `kthread/kthread_wait_tid/zombie_ticks/kernel_entry/kernel_arg`。唤醒 joiner 时不清 `kthread_wait_tid`，只有 `task_reap_kthread()` 成功后才清，避免兜底 reaper 抢在 joiner 前收掉目标。

P8 的内核自测由 control 在启动时执行一次：

- `kthread_create("kping", kthread_demo_entry, &steps, 0)` 得到 TID 4；`kping` 是优先级 0，control 是 3，所以 control 在 `kthread_join` 上一定先进入 BLOCKED，不会让目标在 join 前跑完。
- `kping` yield 192 次，`steps` 递增后 `kthread_exit(5)`；control 被唤醒，join 返回 status 5。
- P8 当时 `kthread_demo_ok` 同时要求 status 5、join 前/后 `kernel_stack_used()` 相等、`join_blocks>0`、`reaped==1`；主页面显示 `kthr 4 s5 OK`。P9 把同一用例扩展为 `kt 4 s5 a1 OK`。
- P8 当时 tasks dump 只剩 control/render/idle 三个槽，slot 3 清零，TID 4 已归还；kthread 计数 `created=1 exited=1 reaped=1 join_blocks=1 join_wakes=1 failed=0`。

QEMU 实测（P8 clean 构建 `kernel.bin=75,336`，`__bss_start=0x92660`、`__bss_end=0x9669C`，`AEVUM_KERNEL_END=0x97000`）：

- 主页面右下 `kthr 4 s5 OK`；`TASK SCHED P3/31` 表示 PID 池只占 control/render/idle 三个。
- P8 当时 `tasks` 数组 16×100 字节：slot 0 control `pid=1 state=RUNNING kthread=0 stack=0x200000`，slot 1 render `pid=2 state=READY stack=0xA000`，slot 2 idle `pid=3 state=READY stack=0xB000`；slot 3 已全零，TID 4 不在 `pid_map`。
- 这说明页面上的 `kthr 4` 不是渲染层伪造的计数：TID 4 确实在运行时被分配，又确实在 join 后消失。

### P9 未 join kthread 的 grace 兜底

P9 只补一个泄漏口：`kthread_exit()` 后没有人 `kthread_join()` 的 ZOMBIE 不再永久占住任务槽、TID 和 4KB 动态栈。

- `task_struct` 增加 `u32 zombie_ticks`；任务在 `SCHED_EXIT` 转 ZOMBIE 时写 `tick_total`，`task_zero()` 清零。
- `kthread.c` 定义 `KTHREAD_ZOMBIE_GRACE_TICKS=100u`。100Hz PIT 下是 1 秒，给 `kthread_create()` 返回 TID 后的创建者留出调用 `kthread_join()` 的窗口。
- 调度器在 `task_reap_detached()` 后、`process_reap_orphans()` 前调用 `kthread_reap_unjoined()`；它把 `task_tick_total()` 和 grace 传给 `task_reap_unjoined_kthread()`。
- `task_reap_unjoined_kthread(now, grace)` 只扫描 `TASK_ZOMBIE && kthread`，跳过 `current_task` 和 `now - zombie_ticks < grace` 的目标；再扫全部非 UNUSED 任务，只要任一 `kthread_wait_tid == target->pid` 就跳过，保证有人正在 join 时绝不抢收。
- 无人等待且过 grace 时，依次 `kernel_stack_free(stack_top)`、`pid_free(pid)`、`task_zero()`，返回回收数量。`kthread_reap_unjoined()` 把数量同时累加到 `reaped_total` 和 `auto_reaped_total`，`live = created - reaped` 仍守恒。
- `task_wake_kthread_waiters()` 只把 joiner 置 READY，不在此处清 `kthread_wait_tid`；`kthread_join()` 在 `task_reap_kthread()` 成功后才清，失败路径也清。否则“已唤醒但还没取走退出码”的 joiner 可能被兜底 reaper 抢先，join 返回 -1。
- 策略边界：grace 过后才来 join 的调用者得到 -1。这不是 join 超时，而是“没有人等在 grace 内”的自动回收；正常 join 仍先拿退出码并回收，不走兜底。

P9 的内核自测仍由 control 启动时执行一次：

- 保留 `kping` 的 join 用例：TID 4、192 次 yield、退出 5；control 在 `task_block_until()` 上实际阻塞。
- 新增 `korph`：只 yield 8 次，然后 `kthread_exit(6)`，故意不 join。
- control 等 140 tick（大于 100 tick grace），再检查 `status==5`、`kernel_stack_used()` 回到创建前、`join_blocks>0`、`reaped==2`、`auto_reaped==1`、`live==0`。
- 主页面右下显示 `kt 4 s5 a1 OK`：`s5` 证明正常 join 没被抢，`a1` 证明兜底路径真的回收了 1 个 kthread，`OK` 是上述整组断言的结果。

QEMU 实测（P9 clean 构建 `kernel.bin=77,160`，`__bss_start=0x92D80`、`__bss_end=0x96DFC`，`AEVUM_KERNEL_END=0x97000`）：

- 串口首帧仍为 `a5 5a 01`，没有 triple fault；主页面右下 `kt 4 s5 a1 OK`，右侧 `TASK SCHED P3/31`。
- `kping` 仍拿到 TID 4 并退出 5；`korph` 退出 6 后先以 ZOMBIE 占住第 5 个 TID/任务槽，超过 100 tick 后被调度器回收。页面 `a1` 是 `stats.auto_reaped`，`s5` 是 join 读到的退出码。
- 这次抓屏没有重新展开 tasks 数组；动态栈是否归还由 `kernel_stack_used()` 前后相等和 `live==0` 的页面断言覆盖，TID/任务槽归还由 `stats.reaped==2` 覆盖。

### P10 删除进程描述符

P10 把 `process_reap()` 从“保留 DEAD 墓碑”改成“保存摘要后清零 PCB”。目标不是删除历史，而是让进程表槽、PID、AS、fd 和任务槽在 wait 回收后全部可复用，同时 V 页仍能读到最近退出记录。

- `struct process` 删除每进程 `reaped[2]`，`proc_zero()` 不再清墓碑字段；`process_reap()` 依次释放线程组、fd、AS、PID，然后调用 `proc_exit_push()`，最后 `proc_zero()`。回收后 `state=PROC_FREE`，`pid/ppid/space/stack_top/signals/fds` 全部归零。
- 新增全局 `exit_ring[PROC_EXIT_HISTORY=4]`，每槽保存 `seq/pid/ppid/exit_code/term_sig/image_id/argc/envc/comm`。`proc_exit_push()` 只复制摘要，不保留任何指针；`process_exit_snapshot(index)` 供 V 页读最新记录。
- `process_reaped_exec(parent_pid, slot)` 改为按 `ppid` 从 ring 找该父进程最近两次回收：slot 1 是最新，slot 0 是次新。`process_image_id()` / `process_exec_counts()` 先查活 PCB，再回退到同 PID 的最新 ring 记录。`proc_reusable_child_pid()` 也从 ring 取该父进程最近退出的子 PID。
- `proc_exit_nth(pid, ppid, nth)` 用 `pid/ppid` 双过滤；`process_wait()` 不再写父进程的 `reaped[]`，退出摘要统一由 `process_reap()` 写入，避免 wait、cleanup、orphan reaper 三条路径各写一份。
- 边界：ring 只保留全局最近 4 条退出，D lane 后续 spawn/thread 轮次会把更早的 P4.2/P4.3 记录挤出；`REAP0/REAP1` 现在表示“该父进程最近两次”，不再保证固定是 P4.2/P4.3。这是固定内存预算下的取舍，不是回收遗漏。

P10 不新增独立内核自测，验收复用 D lane 的真实 fork/exit/wait 序列和 P9 的 kthread 回归：

- 回收前 V 页 slot 4 是 `PID 8 / ZOMB / userE`；父 wait 后同一槽变 `FREE`，`POOL proc 3/12 live 3 zomb 0 dead 0`，没有 DEAD 墓碑残留。
- `SLOT` 页底部 `exit 8 st 9` 来自 exit ring；`ADDRESS` 页选中 PID 7 时 `REAP0/REAP1` 仍返回 `PID 8` 的两条历史，证明 PCB 清零后父进程历史没有丢。
- 主页面右下仍为 `kt 4 s5 a1 OK`，P9 的 join/grace 兜底没有回归。

QEMU 实测（P10 clean 构建 `kernel.bin=77,544`，`__bss_start=0x92F00`、`__bss_end=0x96EBC`，`AEVUM_KERNEL_END=0x97000`）：

- 串口首帧 `a5 5a 01 20...`，没有 triple fault。
- `kernel.bin` 距 98,304B 上限余量 20,760B；BSS 距 `0x97000` 余量 324B。
- `POOL proc 3/12`、`dead 0` 和 slot 4 `FREE` 是本轮 PCB 真正清零的直接证据；`exit 8 st 9` 是 ring 仍保留退出摘要的直接证据。

## 十二、P3.1/P3.2/P4.1/P4.2/P4.3/P4.4/P5.1/P5.2/P5.3/P6.0/P7.0/P7.1/P7.2/P7.3/P7.4/P7.5/P8/P9/P10/P11.1/P11.2/P11.3 边界与已知取舍

- P7.4 期间 `wait` / `join` 仍是 `task_yield_wait()` 轮询；P7.5 已改为 `task_block_until()` 的真正阻塞。P6.0 的参数语义保持不变：`pid>0` 必须先是当前进程的直接子进程，否则立即返回 -1；`pid<=0` 等任意直接子进程。P11.1 已补 `wait_timeout` 与 `sleep`，P11.2 已补 fork/clone 优先级继承；仍没有 EINTR、完整 `WNOHANG` 语义和 `setpriority` 权限组/进程组语义。
- `proc_orphan_children()` 现在把 LIVE 和 ZOMBIE 的直接子进程都改成 `ppid=0`，并写一份全局最近事件；ZOMBIE 子进程仍由 `process_reap_orphans()` 回收。事件只是 V 页只读快照，不改变调度或回收顺序。
- G 页 reparent 面板从“最多 3 个子进程”改为 4 个；`child_count > 4` 时在第 13 行显示 `+N more`，`children` 计数仍显示总数，列表只放前 4 个。
- V 页 TREE 的 `ORPHAN` 行占第 20/21 行，树节点上限从第 21 行收到第 19 行；第 22 行仍是 TREE 统计，第 23/24 行仍是公共底栏。当前用户 demo 没有“父进程先退出、子进程继续存活”的 lane，所以 TREE 页默认显示 `ORPHAN none yet`；事件路径已由 `process_terminate_current()` / `process_exit_current()` 的真实退出路径调用，不是渲染层伪造。
- 当前速度基线（QEMU TCG，非实时）：100 Hz PIT 是硬控制节拍上限；CPU 页 4.14 秒窗口内 `SWITCHES +129,890`（约 31.4k/s）、`PREEMPTIONS +384`（约 92.8/s），control/render/idle 仍是 50/38/12；监视器建立后 69 ms 的首帧采样已有完整机械臂画面。详见 `PERFORMANCE.md`。
- P5.2 的 `V` 页固定 4 页，原第 5 页 `DEMO` 因 `kernel.bin` 超过当时 65,536B 上限被删除，旧证据压缩进 `ADDRESS` 页。P10 clean 构建为 77,544B、上限 98,304B、余量 20,760B；`--gc-sections` 已回收不可达代码，但新增页面或长文案前仍必须先算体积。P10 的 BSS 只剩 324B，下一阶段不能再随手加静态大数组。
- `V` 页的页面状态是单个静态实例（`page_mode/page_slot/page_first`），四页共享同一个选中槽；`process_slot_space()` / `process_slot_fd_info()` 是只读快照，没有 seqlock，单核下仍可能在 PIT 切换窗口看到半更新字段，已知取舍是“可观察面优先、不做事务化快照”。
- `PgDn` 把选中槽带到槽 10/11 后，空槽详情只显示 `EMPTY` 和池统计；要回到有内容槽需 `Up` 或 `Home`，这是当前交互语义，不是滚动丢失。
- P5.3 没有新增持久对象、页面或第二套计数；只复用 P5.1 池页与用户数据槽，压测状态由 ring3 写、页面只读。`S` 与 `Y` 共用同一池页外壳，二者互斥，不能同时运行。

- P7.1 线程组只做最小闭环：`clone` 只支持 `VM/FILES/SIGHAND/THREAD`，不支持 `CLONE_FS`、TLS、futex 或 per-thread signal pending；P11.2 已让 clone 继承当前线程优先级；P7.5 起 `thread_join` 走 `task_block_until()` 阻塞，不再用 `task_yield_wait()` 轮询。线程故障默认会通过 `process_terminate_current()` 终止整个线程组，目前只按代码路径设计，尚未做 worker 触发 `#PF/#DE` 的边界测试。
- P7.2 的 `thread_detach` 只是生命周期控制：没有 detach 时仍必须 join；detached 线程的退出码不可再读取；`task_reap_detached()` 依赖下一次调度事件，不会在 `thread_exit` 返回前释放当前栈。
- P7.3 已覆盖 join/detach 的 0、-1、self、不存在 TID，但未覆盖“跨线程组 join 一个真实存在的 TID”、detach 一个已 ZOMBIE 的线程、以及主线程 detach 自己后退出；这些仍由同组 `proc` 校验和 detached 延后回收兜底，尚未做成独立断言。
- P7.4 已覆盖主线程 ZOMBIE 留在组内、由最后线程退出触发 `process_reap()` 整组释放的变体，并确认 tasks 表里该子进程整组任务槽全部消失。未覆盖：worker B 不 join A、A 先 exit 5 但仍以 ZOMBIE 留在组内直到整组回收；detached worker 故障终止整组；以及主线程 ZOMBIE 被单独 detach。
- P7.5 已覆盖 `wait` 与 `thread_join` 的“目标仍 LIVE 时阻塞、随后被唤醒”正向路径，并用全局计数证明阻塞与唤醒都发生。未覆盖：等待期间收到信号后的 EINTR、`wait`/`join` 超时、多个父/子/线程同时等待同一目标、以及目标在唤醒后再次变成 LIVE（PID/TID 复用）的竞态；当前依赖 `ready()` 在外层重查后走完整扫描或错误返回。
- P8 的 kthread 只做单创建者、单 joiner 最小闭环：支持 create/exit/self/join/stats，不支持 detach、超时、取消、优先级继承或 kthread 之间组成线程组；未 join 的 kthread ZOMBIE 已由 P9 的 100 tick grace 兜底回收，但晚于 grace 才 join 会得到 -1。`kthread_create` 只允许 ring0 任务调用，用户态没有入口；kthread 不创建 PCB、不进入 process 家族回收路径。未做多等待者、TID 复用竞态和高频创建/销毁压力测试。
- P9 clean 构建为 77,160B、上限 98,304B、余量 21,144B；`__bss_start=0x92D80`、`__bss_end=0x96DFC`，BSS 距 `AEVUM_KERNEL_END=0x97000` 仅 516B。每个任务槽从 P8 的 100B 增至 104B（`task_struct` 新增 `zombie_ticks`），16 槽共增加 64B；继续加内核数据结构前必须先核算 BSS 和镜像体积。
- P9 只验证“单创建者、无 joiner”的自动回收：多 joiner 等同一 TID、join 超时/取消、TID 复用竞态、kthread 故障终止、worker 池和高频创建/销毁仍未覆盖。`task_reap_unjoined_kthread()` 每轮调度扫 16 个任务槽，没有做单独延迟测量；`task_struct` 的 `zombie_ticks` 只用于计龄，不参与优先级或调度排序。
- P10 的 exit ring 是全局固定 4 槽，不按父进程或 PID 分桶；高并发 fork/wait 时早期退出摘要会被覆盖，V 页 `REAP0/REAP1` 只保证“最近两次”，不保证任意历史。ring 不做事务化快照，单核 PIT 窗口内 `process_exit_snapshot()` 与 `process_reap()` 并发时可能读到前一条 `seq` 的部分字段；当前接受“可观察面优先”。PCB 清零后 `process_snapshot()` 对该槽返回 0，V 页不再显示 DEAD 行；信号死因改由 ring 的最新生效记录保留。
- P7.1 的多线程 `exec` 采取拒绝策略，不杀其他线程；长期是“拒绝”还是“exec 杀死同组其他线程”尚未定。线程 TID 与进程 TGID 共用 `PID_MAX=31`，且各占一个任务槽和 4KB 内核栈，`PROC_MAX=12/TASK_MAX=16/KSTACK=12` 下多线程会快速消耗容量。

- P3.2 已有用户 handler、signal frame 和 `sigreturn`；P4.1 已有内嵌 flat 镜像 `exec`、PID/任务槽保留、AS 替换和信号重置；P4.2 已有 `argv/envp` 解析、字符串迁移、SysV 初始栈和 `exec_argc/exec_envc` 元数据；P4.3 已有每进程 fd 表、匿名 pipe、`pipe/read/write/close` 和 exec 后 fd 保留；P4.4 已有全局 console fd 0/1、fork 继承、close 边界和 VGA/COM1 双写。仍没有 ELF loader、文件系统/路径 `open`、console 会话/前台焦点、`kill()`、信号排队、真正阻塞的 mask、`SA_SIGINFO`/`SA_RESTART` 等 POSIX 语义。
- `sig_mask` 只随 frame 保存/恢复，不阻止信号投递；handler 执行期间同一信号仍可能再次进入。
- `#DE` 解码只覆盖 `F6/F7` 的 `div/idiv` group，且只处理可读的用户代码字节；其他同步异常把原 EIP 存入 frame，handler 若直接 `ret` 会回到 faulting 指令，需要自行修改 frame 的 `eip`。
- `SIG_IGN` 只对可安全前移的 `#DE` 和保存 EIP 已指向下一条的 `#BP` 生效；`#UD/#GP` 的忽略会退回默认终止，避免死循环。
- `#PF` 仍由 `page_fault.c` 独占，`exception.c` 不安装 14 号门；两者只在“用户不可恢复故障最终按 SIGSEGV 终止”这一语义上对齐。
- COW 拆分后如果旧页引用已降到 1，当前实现仍会复制一次；这是为了保持“拆分函数只做复制”的最小语义，后续可按引用计数改成原地恢复 RW。
- 第二次 fork 的 COW 继承修复只保证已有 COW 页不退化；没有改变 code 页只读共享、data/stack/heap/control 走 COW 的边界。
- `fork` 的 code 页只读共享，不参与 COW；data/stack/heap 和 control 走 COW。
- A/B/C/D/F 由 render 内核任务通过 `process_create_user()` 创建；E1/E2/E3/E4 是用户父进程 `fork` 创建的进程。
- `family.c` 只维护内核任务树，用户进程树由 `process.c` 维护；`G` 页和 `V` 页是两套树。
- `exec` 只接受内嵌镜像 1/2，不读盘、不解析 ELF；`argv/envp` 最多 4 项、每串最多 31 字符，必须有 NULL 结尾。fd 表只接匿名 pipe，pipe 池固定 4 个对象、每对象 64 字节且非阻塞；没有文件系统、路径 `open` 或 console fd 0/1。用户堆已有 brk 和按需零填，但不是完整 `mmap`。
- exec 的参数解析、新 AS 分配或新栈构造失败时返回 -1，旧 AS、旧帧和 `image_id` 都不动；成功后旧 AS 已释放，不再保留旧页。`exec` 不改变 PID/PPID/父进程 notify 关系。
- 非探针内核 trap 仍是终止性诊断；用户态 `#PF` 与 `#DE/#UD/#GP` 已分发，但 CPL0 的 `#DE/#UD/#GP` 没有 fixup，直接停机。
- VGA BIOS 影子 ROM 位于 `0xC0000..0xFFFFF`，物理池必须避开；当前池选在 `0x100000..0x1FEFFF`，不与内核、VGA、任务栈重叠。

P3.1 的目标是让“同步异常 -> 致命信号 -> ZOMBIE 墓碑 -> 父进程 wait status + SIGCHLD”这条默认动作链路先成立；P3.2 的目标是让用户 handler 接管同步异常并在 `sigreturn` 后继续原指令；P4.1 的目标是在不换 PID/任务槽的前提下，把当前进程从 ring3 镜像切到 exec 镜像并释放旧 AS；P4.2 进一步让 `argv/envp` 跨过 exec 并落在新用户栈；P4.3 在不引入文件系统的前提下，让 fd 表随 fork 复制、随 exec 保留、随 exit 关闭，并用匿名 pipe 验证父子进程数据通路。五条链路当前都已成立并有 QEMU 证据。

## 十一、P11.1 定时阻塞与 P11.2 优先级生命周期

P11.1 在不改调度器单核结构的前提下，给 `TASK_BLOCKED` 增加一个可选的 PIT 到期点。这样 `wait` 不再只能永久阻塞，用户态也能用 tick 做粗粒度睡眠。

- `struct task_struct` 在 `kthread_wait_tid` 后新增 `u32 wake_tick`。值为 0 表示普通阻塞，不参与定时扫描；非 0 表示绝对 tick deadline。实测 `sizeof(struct task_struct)=0x6C=108B`，`TASK_MAX=16` 共 1,728B；BSS 总量 `0x3FFC`。
- `task_block_until_timeout(ready,ctx,ticks)` 在进入时固定 `deadline=tick_total+ticks`，`ticks` 上限 `0x7FFFFFFF`；在 `pushf; cli` 窗口内循环：先 `ready(ctx)`，条件满足返回 0；再 `(int)(tick_total-deadline)>=0`，到期返回 1；否则写 `TASK_BLOCKED`、`wake_tick=deadline`、`SCHED_BLOCK` 后 `int $0x40`。虚假唤醒不会重置 deadline，条件始终优先于超时。
- `task_schedule_irq()` 的 IRQ0 分支在 `tick_total++` 后、任务记账前调用 `task_wake_expired(tick_total)`：扫描所有 `TASK_BLOCKED && wake_tick` 任务，用有符号差防 32 位回绕，到期改 `READY`、清 `wake_tick`、`timeout_wake_total++`。
- 所有正常唤醒路径（`task_wake()`、`task_wake_process()`、`task_wake_kthread_waiters()`、`task_set_current_state()` 的非 RUNNING 转换）都清 `wake_tick`，避免旧 deadline 把后续普通阻塞提前唤醒。
- `task_sleep_ticks(ticks)` 不查询条件，直接走超时阻塞；`ticks=0` 立即返回 0。`task_timer_stats()` 导出 `sleep_block_total / sleep_wake_total / timeout_wake_total`，C 页显示 `SLEEP b <blocks> w <wakes> tmo <timeouts>`。
- `process_wait_timeout(pid,user_status,ticks)` 与 `process_wait()` 共用 `process_wait_common()`：先扫描 ZOMBIE 并 `process_reap()`，没有可回收子进程时才进入超时阻塞。返回子 PID 表示成功，`0` 表示超时，`-1` 表示参数/状态错误；`ticks=0` 等价一次非阻塞轮询。
- 超时不会移除子进程：慢子进程仍在 `sleep` 时第一次 `wait_timeout(5)` 返回 0，父进程随后用 `wait_timeout(50)` 仍能拿到同一子 PID 和 status 7；第二次 wait 证明“条件满足优先于超时”。快子进程 `sleep(1)` 后 exit(9)，`wait_timeout(20)` 直接拿到 PID/9。
- ABI：`eax=24 sleep(ebx=ticks)` 返回 0/-1；`eax=25 wait_timeout(ebx=pid, ecx=user_status, edx=ticks)` 返回子 PID/0/-1。`ticks` 是 PIT tick；当前 PIT 100Hz，所以 1 tick = 10ms。
- D lane 顺序：P7.5 阻塞验收写 `WBLK` 后进入 timer 轮；先 fork 慢子并做 5 tick 超时，再 50 tick 回收；再 fork 快子做 20 tick 回收。全部通过写 `0x401250="TMR1"`，V 页 ADDRESS 显示 `tmr OK`。
- 构建：clean `kernel.bin=78,536`、上限 98,304、余量 29,768B；`__bss_start=0x932E0`、`__bss_end=0x972DC`；`AEVUM_KERNEL_END` 修正为 `0x98000`（boot 实际读 192 扇区到 `0x98000`），BSS 余量 3,364B。`struct task_struct` 从 104B 增至 108B，16 槽 +64B；`.text=0x113A0`、`.rodata=0x1E38`、`.data=0xE8`、`.bss=0x3FFC`。
- QEMU：D 页 ADDRESS `tmr OK`、`CHILDREN live 0 zomb 0`、`blk 6 /1`；C 页 `SLEEP b 2 w 2 tmo 3`；P5.3 复测 `round 100/100 first 5 -> 5 live 1 state STRESS OK`、`PHYS 6/255`；主页面 `kt 4 s5 a1 OK`；串口无 fault/panic。
- 边界：超时目前只按 PIT tick，不是高精度时钟；`wait_timeout` 未处理 EINTR/WNOHANG 的完整 POSIX 语义，`ticks=0` 是非阻塞轮询但不是 `WNOHANG` 返回值约定。`sleep` 不可被信号提前打断；超时后不会自动杀子进程；多等待者同时等同一目标仍未覆盖。若继续加字段，BSS 余量足够但 `struct task_struct` 已到 108B，优先复用字段而不是扩静态数组。

### P11.2 优先级读写与 fork/clone 继承

P11.2 不改变 `priority 0..3 -> weight 4..1` 的既有映射，只把优先级从“创建时参数”变成可由用户态读写的生命周期属性，并修正 fork/clone 的继承路径。

- `task_priority_get(tid)` 与 `task_priority_set(tid,priority)` 按 TID 操作；`tid=0` 表示当前任务。优先级范围 0..3，0 最高。get 成功返回 0..3，set 成功返回 0，参数/目标/状态错误返回 -1。
- `task_priority_set()` 在 `cli` 窗口内完成查找与写入；只改后续 PIT 记账权重，不重算历史 `vruntime`，避免反复 setpriority 清零累计虚拟时间刷调度优势。ZOMBIE 任务不接受 set；用户态只能改 `user` 任务，`setpriority(1,0)` 这类对 control 的请求返回 -1。
- `task_create_common()` 仍校验 0..3；`task_fork_user()` 从当前父任务取优先级，`task_create_thread_user()` 从当前线程取优先级。`process_clone_thread_current()` 删除原先被忽略的 `priority` 参数，clone 不再硬编码 1。
- ABI：`eax=26 getpriority(ebx=tid)` 返回 0..3/-1；`eax=27 setpriority(ebx=tid,ecx=priority)` 返回 0/-1。`tid` 是任务 TID，不是进程组 ID；对进程主线程用其 TID 即可。
- D lane 验收：timer 轮先读默认 1，set 3 后读回 3，set 4 返回 -1，set 1 内核任务返回 -1；fork 慢子后 `getpriority(child)==3`，父恢复 1 再跑原 timer 用例；P7.1 clone 前父线程设 2，clone 后 `getpriority(TID)==2`，join 后父恢复 1。全部通过写 `0x401260="PRI1"`，V 页 ADDRESS `TASK READY P1` 并显示 `pri OK`。
- 构建：clean `kernel.bin=79,016`、上限 98,304、余量 29,288B；`__bss_start=0x934C0`、`__bss_end=0x974BC`，BSS 余量 2,884B；`.text=0x11568`、`.rodata=0x1E3C`、`.data=0xE8`、`.bss=0x3FFC`。`sizeof(struct task_struct)` 仍为 108B，未新增 BSS 字段。
- ring3 仍受 4KB code VMA 限制：P11.2 验收加入后 `code_size` 一度到 4,168B，已把 48 条小立即数加载压成 `push byte/pop`，并把线程/组/压测轮的多条零初始化改为 `xor eax,eax` 后复用 `mov [addr],eax`，最终 `code_size=4,091B`，余量 5B。
- 边界：没有 `setpriority` 的权限组/进程组语义，用户可改任意 user 任务；没有 nice 值、没有优先级上限继承、没有 PI/futex；优先级变更不触发立即抢占，下一次 PIT 选择才生效。

## 十三、P11.3 对象等待队列与阻塞 I/O

P11.3 在不改 `read/write` 既有返回约定的前提下，把阻塞点从“任务自己轮询”提升为“对象等待队列 + 条件重查 + tick 超时”。旧 `eax=15 read` / `eax=16 write` 仍然是原来的非阻塞接口：pipe 空读且写端仍活着返回 -1，写端全关返回 0，pipe 满写返回 -1；需要等待的调用走新增的 `28/29`。

### 等待队列

- `include/kernel/task.h` 新增 `struct wait_queue { u8 wake_seq; }`。`wake_seq` 只用于唤醒序号/调试，不承担 FIFO 排队；原设计用 u32，clean build 时 BSS 越过 `AEVUM_KERNEL_END` 0x1C，缩成 u8 后回到预算内。
- `task_struct` 增加 `struct wait_queue *waitq`；`sizeof(struct task_struct)` 从 108B 增至 112B，16 槽多 64B。`waitq=0` 表示普通阻塞，不参与对象广播。
- `task_block_on(q, ready, ctx, ticks)` 在 `pushf; cli` 窗口内循环：先调 `ready(ctx)` 重查真实条件；条件满足返回 0；`ticks>0` 时再查绝对 deadline，到期返回 1；两者都不成立才写 `TASK_BLOCKED`、`wake_tick=deadline`、`waitq=q`，然后 `int $0x40` 让出。虚假唤醒只回到循环重查，不会重置 deadline。`ticks=0` 只查询一次，不置 BLOCKED。
- `wait_queue_wake_all(q)` 在关中断窗口内扫描 16 个任务槽，把 `state==TASK_BLOCKED && waitq==q` 的任务改 READY，并清 `wake_tick/waitq`；它只改状态，不直接切任务，真正的切换仍由调度器完成。没有等待者链表、没有 FIFO、没有优先级排序，因此唤醒是广播后由各等待者重查条件。
- `task_block_until_timeout()`、`task_wake_expired()`、`task_wake()`、`task_wake_process()`、`task_wake_kthread_waiters()`、`task_zero()` 都补了 `waitq` 清理，避免任务被普通唤醒后还挂在上一个对象的广播集合里。
- `task_io_stats(blocks,wakes,timeouts)` 只统计 `task_block_on` 路径；C 页显示 `IO b <blocks> w <wakes> tmo <timeouts>`，与 P11.1 的 `SLEEP` 统计分开。

### pipe 与 console

- `struct pipe` 增加 `read_wq/write_wq`，在 `pipe_create` 初始化。`pipe_read_block(p,buf,len,ticks)`：`ticks=0` 直接走原 `pipe_read()`；`ticks>0` 时若 `count==0 && writers>0` 就在 `read_wq` 上等，写端全关时立即按 EOF 返回 0。`pipe_write_block()`：容量不足且仍有读者时在 `write_wq` 上等，`readers==0` 立即返回 -1。
- `pipe_read()` 读出后 `wait_queue_wake_all(&p->write_wq)`；`pipe_write()` 写入后 `wait_queue_wake_all(&p->read_wq)`；`pipe_release()` 在最后一个写端关闭时唤醒读者、最后一个读端关闭时唤醒写者、最后引用释放时两个队列都广播。这样阻塞读者不会因写端退出而永久挂起。
- `console.c` 增加静态 `input_wq`；`console_input_push()` 每压入一个字节就广播，`console_read_block()` 在输入为空且 `ticks>0` 时等待，超时返回 -2。console 的输出路径没有阻塞语义，写 fd 1 仍是同步写 VGA/COM1。

### 用户 ABI 与 fd 层

- 新 ABI：`eax=28 read_block(ebx=fd, ecx=user_buf, edx=len, esi=ticks)`；`eax=29 write_block(...)`。成功返回字节数，pipe EOF 返回 0，参数/端点错误返回 -1，超时返回 -2。`ticks=0` 等价原非阻塞调用。
- `syscall.c` 的 case 28/29 把 `frame->esi` 作为 ticks 传入；`proc_fd_read_block()` / `proc_fd_write_block()` 先做 fd kind、用户缓冲区和长度校验，再 `copyin/copyout` 到内核临时缓冲，调用 pipe/console 阻塞接口。用户指针不能直接进 pipe 环形缓冲，避免内核解引用用户地址。
- fd 表、pipe 端点计数和 fork 继承沿用 P4.3/P4.4：fork 后父子各自持有读/写端引用，close 只减自己那一份；pipe 的 `readers/writers` 决定 EOF 和 EPIPE 边界。

### image 3 验收镜像

- P11.3 新增独立 flat 镜像 `user/exec-io/exec_io.asm`，固定链接在 `0x400000`，由 D lane 用 `eax=18 spawn(image_id=3, argv=0, envp=0)` 启动；父 ring3 只检查 image 3 的退出码。没有把三段阻塞用例塞进 ring3，因为 ring3 的 4KB code VMA 只剩 35B。
- 第一段：建 pipe、保持写端活着，空读 `read_block(fd,buf,4,2)` 必须返回 -2，验证 tick 超时。
- 第二段：建 pipe，数据页预写 `IORD`；`fork` 后子进程关闭读端、`sleep(20)` 再从 data 段写 4B；父关闭写端后 `read_block(...,500)` 必须返回 4 且内容为 `IORD`，再 `wait(child)` 得 status 0。读超时从 50 放宽到 500 tick，是因为 QEMU TCG 下子进程 sleep 后的调度窗口偶尔超过 50 tick；这不是条件丢失。
- 第三段：建 pipe、`fork`；父关闭写端但保留自己的读端，子关闭写端并 `sleep(100)` 后读 1B；父用 8 次 `write(fd,buf,8)` 填满 64B，再 `write_block(fd,buf,1,500)` 必须返回 1；子读到 1B 后 exit 0，父 wait 得 status 0。父若在 fork 后立即关自己的读端，pipe 会在父子 close 时序之间出现 `readers==0`，`write_block` 合法返回 -1；因此验收镜像刻意保留父读端。
- pipe payload 全部放 data 段（`0x401010`、`0x4010F0`），不能从 code 段 `io_word + 0x400000` 做 `copyin`：code VMA 只读，`address_space_copyin()` 会拒绝。image3 最终 644B（`user_io_exec_end=0x284`），成功写 `0x401040="IO13"`、console `"P11.3 IO OK\n"` 并 exit(0)，失败写 `"IOBAD\n"` 并按 1..12 退出。
- D lane 为 image 3 预留 `0x4012A0="IOB1"` 和 `0x4012A4` wait status；成功时 V 页 ADDRESS 底部显示 `io OK`，exit ring 记录 `REAP1 <pid> img3 a0 e0`。

### 构建与 QEMU 证据

- clean build：`kernel.bin=81,768B`、`boot.bin=512B`、`disk.img=1,474,560B`；`.text=73,769B`、`.rodata=7,744B`、`.data=232B`、`.bss=16,476B`；`__bss_start=0x93F80`、`__bss_end=0x97FDC`，距 `AEVUM_KERNEL_END=0x98000` 余量 36B。
- SHA256：kernel `940151ec21468b1a5eaf063c8c5e7f5b8bb9c98339b56adecba470cff60070dc`、boot `218ecc892630f5f723b8e3228898714b18d82bf31a4ad31071f9a3480b6ce3d2`、disk `5c435358e58f9008b0599935dd6529abc05a47b5b173665f72442c1e21cf8c55`。
- ring3 加入 P11.3 lane 后 `code_size` 一度到 4,169B；通过复用零初始化、首次 flag 改 `inc dword [flag]`、`.child_main_exit` 的 8 条 mov 改 xor+mov 等压缩，最终 `code_size=4,061B`（`user_program_end=0xFDD`），4KB 余量 35B。
- QEMU 最终证据：V 页 ADDRESS 选中 PID 7 显示 `io OK`、`REAP1 6 img3 a0 e0`、`CHILDREN live 0 zomb 0`；C 页 `IO b 3 w 2 tmo 1`、`SLEEP b 4 w 4 tmo 6`；P5.3 复测 `round 100/100 first 5 -> 5 live 1 state STRESS OK`、`PHYS 6/255`、`proc/AS/KSTK 1/12`；主页面仍为 `kt 4 s5 a1` 基线；串口无 fault/panic。

### 边界

- 等待队列是 16 槽全表广播，不是 per-object 链表：一次唤醒 O(16)，没有 FIFO 公平性，也没有按优先级选择；多个等待者同时被唤醒后由调度器重新选，可能重复抢同一条件。
- `ticks` 仍是 PIT tick 粒度（100Hz=10ms），不是高精度 deadline；超时与 P11.1 共用全局 `timeout_wake_total`，image3 的等待超时会进入 C 页 `SLEEP tmo` 口径，这是当前已知的统计混用。
- pipe 的 `writers/readers` 计数跟 fd 引用走，但 fork/close 时序仍可能让写者在边界处看到 `readers==0`；这是“端点已关”的真实语义，不是 lost wake。没有 `poll/select`、没有 EINTR、没有非阻塞标志位、没有部分写保证。
- console 只有输入队列可阻塞；输出没有队列满/背压路径。image 3 是独立 4KB code VMA，不参与 ring3 的 code_size 预算，但 spawn 仍固定 priority 1、不继承父优先级。