# 32 位裸机多进程内核（教学版）

> 本目录是 AEVUM 桌面「内核实况」窗口实际运行的内核。原 `kernel.c` 已按
> 「归属唯一、失败可见」拆成模块目录；`kernel.bin` 仍由本目录 `Makefile` 产出。
> COM1 串口初始化和 VGA 差量帧上报保留：每轮渲染后把变化的 80×25 单元按
> `A5 5A 01 COUNT [row col ch attr] CRC` 发到 QEMU 串口，由
> `src/kernel/host/bridge/serial_bridge.py` 转成 WebSocket。没有这段串口旁路，
> 内核只写 `0xB8000`，桥接收不到画面。
>
> 现在同一 COM1 发动态关节状态帧
> `A5 5A 03 COUNT + COUNT×i32 LE + CRC`（`COUNT = 3n + 10`，n=3 时 19 字段，n=8 时 34 字段），
> 并接收 `J` + u8 n + n×int16 LE（0.001 rad）关节目标、`B` + u8 n + n×4×i32 LE 身体描述
> 和 `R` 释放手动；身体描述由宿主下发，默认仍是 3R（0.30/0.30/0.20 m）。
> 手动模式按 PIT tick 保持 5 秒，作为桥接断开时的兜底。
> Forge 可显示 q/dq/q_target、末端误差、可操作度、sigma、null share 和 limit margin。
>
> 构建（Windows 上可用 WSL2，或任意 Linux）：
>
> ```sh
> make clean && make all
> ```
>
> `interface/desktop/main.js` 优先启动本目录的 `disk.img`；`kernel.bin` 上限
> 118,784 字节（232 个扇区；`boot.asm` 分两段 LBA 读：128 扇区到 `0x80000`，104 扇区到 `0x90000`）。
> 当前构建 98,952 字节，距离 118,784 字节窗口余 19,832 字节。串口快照放在 `0x30000`，
> 不占内核镜像窗口。模块归属、结构化错误和验收流程见 `CONSTRUCTION.md`，进程上下文完整化路线见 `PROCESS.md`。

一个用 NASM + C 写的 32 位裸机内核：从软盘镜像启动，进入保护模式、建立自己的 GDT 和分页，
然后运行 `control` / `render` / `idle` 三个常驻任务；按 `P` 可生成一个 `probe` 任务，按 `T` 可启动两个 CPU 密集型任务的抢占决斗，按 `F` 可打开 FPU/SSE 上下文实验，按 `C` 可切到全屏 CPU 时间统计。PIT 100Hz 时间片抢占、按优先级加权的 `vruntime` 选择和任务主动让出共同驱动轮转；按 `V` 可切到 COW fork/brk/guard/物理页池/同步信号与用户 handler 演示，按 `X` 可切到 console fd 0/1 页，按 `K` 触发内核态 `#PF` 受控探针，页内 `R` 恢复、`H` 停机；整个过程都在 VGA 文本模式下绘制可由宿主描述驱动的平面串联链（默认 3R）和运行信息。

---

## 一、功能简介

- **启动流程**：BIOS 把引导扇区加载到 `0x7C00` → `boot.asm` 用 `INT 13h` 的 LBA 扩展分两段读盘（128 扇区到 `0x80000`，104 扇区到 `0x90000`）
  → 装载 GDT 进入保护模式 → 建立 0~4MB 恒等分页 → 跳到 `0x80000`。
- **内核入口**：`loader.asm` 加载内核自己的 6 项 GDT、清 BSS、把主栈设为 `0xC000`（启动镜像里不再需要尾部搬运），再调用 C 的 `kernel_main`；`tss_init()` 写 TSS 描述符并 `ltr 0x28`。
- **多进程**：`kernel_main` 通过 `task_create` 为 `control` / `render` / `idle` 构造独立内核栈；PID 由 `task.c` 的位图自动分配，应用不再硬编码，再用 `task_start` 切入第一个任务。
  每个进程各自拥有栈和计数器，互不影响。
- **上下文切换**：IDT 门 + 统一中断帧保存 `eax/ebx/ecx/edx/esi/edi/ebp/eip/cs/eflags`；每个任务栈底预留 512 字节 FXSAVE 区，切换时用 `fxsave/fxrstor` 保存 FPU/SSE 状态；`loader.asm` 打开 CR4.OSFXSR/OSXMMEXCPT 并清 CR0.EM/TS。PIT 通道 0 以 100Hz 触发 IRQ0，`task_schedule_irq` 在 IRQ0 与 `int 0x40` 两条入口上做同一套调度；若选中的仍是当前任务，必须恢复 `RUNNING`，否则一次 yield 会让任务永久停在 `READY`、PIT 不再给它计费。
  `task_yield()` / `task_block()` / `task_exit()` / `task_start()` 都走 `int 0x40`；`task_exit()` 置 ZOMBIE；内核任务由调度器回收，用户进程由父进程 `wait` 或 reaper 回收；`task_block()` / `task_wake()` 已打通 BLOCKED 与 READY。
  `task_set_current_state(state)` 是当前任务状态的统一写入口：`RUNNING` 无副作用，`READY` / `BLOCKED` / `ZOMBIE` 分别等价于让出、阻塞、退出；`task_yield()` / `task_block()` / `task_exit()` 都只是它的薄封装。
- **FPU 上下文实验**：按 `F`（扫描码 `0x21`）创建 `fpuA` / `fpuB` 两个 CPU 密集型任务；两个任务都没有 `task_yield()`，各自在 `XMM0` 里保存不同标签和计数。全屏显示两条 lane 的 PID、TAG、COUNT、`CTX OK/LOST` 和进度条；如果 `fxsave/fxrstor` 漏掉，标签会串到另一条 lane，屏幕直接判 `CTX LOST`。再按 `F` 停止并回到机械臂。
- **进程家族树**：按 `G`（扫描码 `0x22`）进入全屏 `AEVUM / PROCESS FAMILY TREE`；`famP` 创建 `famA` / `famB` 后主动退出，两个直接子进程由 `family.c` 重挂到 `idle`。树形页显示真实的 `ppid` 关系、每任务 CPU、reaper 和最近一次 `parent -> reaper` 重挂事件；再按 `G` 停止并回到机械臂。
- **用户同步异常与信号**：`#DE/#BP/#UD/#GP` 由 `arch/x86/cpu/exception/exception.c` 唯一分发；CPL3 命中后记录 `pid/eip/err/cr2`，按 `#DE -> SIGFPE`、`#BP -> SIGTRAP`、`#UD -> SIGILL`、`#GP -> SIGSEGV` 先查每进程 `sigactions[]`。没有 handler 时走 P3.1 默认终止：`process_terminate_current(sig)` 把进程置 ZOMBIE，wait status 为 `0x80000000 | sig`，所以默认 `#DE` 返回 `0x80000008`。有用户 handler 时在用户栈构造 signal frame，`iret` 进 handler；handler `ret` 到 trampoline，`eax=12 sigreturn` 恢复原 GPR/EIP/CS/EFLAGS/ESP/SS。`#DE` 的保存 EIP 指向 faulting `div/idiv`，内核做最小 `F6/F7` 解码后前移到下一条指令。`sigaction` 是 `eax=11`，mask 只随 frame 保存/恢复，不做阻塞；`fork` 继承 handler 表与 mask，不继承 pending。V 页显示 `EXIT=SIG11/SIG8`、`#DE pid/eip/sig`、`hdl` 和 `SIGCHLD n pend`。
- **用户 exec**：`eax=13 exec(image_id, argv, envp)` 保留当前进程的 PID、PPID 和任务槽，只替换用户地址空间和用户镜像。`process_exec_current()` 先在旧 AS 仍有效时解析 `argv/envp`（最多 4 项、每串最多 31 字符、必须有 NULL 结尾），再建新 AS，把字符串复制到新用户栈并按 `argc, argv[], NULL, envp[], NULL, 字符串区` 布局；2 项参数/2 项环境时 `esp=0x403FC8`。随后 `cli` 下切 CR3、更新 `task_struct` 持有的 AS 指针，重置 `sigactions`、mask、pending，把 iret 帧改成入口 `0x400000`，最后释放旧 AS；解析、AS 分配或栈构造失败时返回 -1，旧 AS/frame 不动。`exec` 完全不触碰 fd 表。`user/exec/exec.asm` 是第二个固定链接在 `0x400000` 的 flat 镜像，从新栈逐字节校验 `alpha/beta/MODE=p42/LEVEL=2`，成功后写 `ARGV/ENVP` 并 `exit(10)`；失败码 20..25 分别对应 argc/argv0/argv1/envc/env0/env1。
- **P7.0 直接进程创建（spawn）**：`eax=18 spawn(image_id,argv,envp)` 不经过 `fork`，直接从当前 ring3 进程创建独立子进程。`process_spawn_current()` 从父 AS 解析参数，分配新 PID/PCB/内核栈/AS，把参数复制到新栈，复制父 fd 表，再以 `user_eip=0x400000` 和新的 `user_esp` 发布任务；成功返回子 PID，失败 `-1`。子进程的父进程仍是调用者，父进程用 `wait` 回收。D lane 在 P4.3 pipe 之后直接 spawn image 1，子进程校验 `alpha/beta/MODE=p42/LEVEL=2` 后 `exit(10)`，父 wait 到 10 并在 V 页写 `SPAWN OK`；这条路径证明“创建进程”不必先复制父地址空间。
- **每进程 fd 表与匿名 pipe**：`kernel/proc/fd/fd.c` 是 8 槽 fd 表的唯一 owner；fd 0 为 console 读、fd 1 为 console 写，pipe 从 fd 2 起分配。fd 表用 kind 区分 `PIPE_READ/WRITE` 与 `CONSOLE_READ/WRITE`：`fork` 复制整张表并对 pipe 端点 `pipe_acquire()`，console 端点直接继承；`exec` 保留 fd 表；`exit`、信号终止和 `reap` 都 `proc_fd_table_close_all()`，只对 pipe kind 调 `pipe_release()`。`kernel/ipc/pipe/pipe.c` 固定 4 个 pipe 对象、每对象 64 字节非阻塞环形缓冲，维护 `refs/readers/writers`：空读且仍有写端返回 `-1`，写端全关返回 `0`，满写返回 `-1`，最后一个引用释放后对象立即回池。系统调用 `eax=14 pipe(ebx=user u32[2])`、`15 read(fd,buf,count)`、`16 write(fd,buf,count)`、`17 close(fd)`；用户指针校验和 copyin/copyout 都在 fd 模块内完成。`user/exec-pipe/exec_pipe.asm` 是 image 2：从 `argv={"pipe","writer","FD=3"}` 取 fd，写 4 字节 `PIPE` 后 `exit(12)`；失败码 26..31。
- **console fd 0/1**：`drivers/console/console.c` 是 console 字符设备的唯一 owner。输入把 scancode set 1 翻成 US ASCII，写入 16 字节非阻塞环形队列；`read(0)` 空队列返回 `-1`，有数据返回 1..len。输出最多 64 字节一次，追加到 8 行×64 字符行缓存，同时原样 `uart_write()` 到 COM1；X 页把行缓存渲染到 VGA。新用户进程创建后安装 fd 0 读、fd 1 写；fork 复制父表，exec 保留 fd 表，close 只清调用者的句柄。X 页父进程 fork 后关闭自己的 0/1，`read(0)`/`write(1)` 均返回 `-1`，子进程继续用继承端点读键并写 `CON:<key>` / `FD01 OK`，证明多进程共享 console、close 不影响其他句柄。
- **V 页 4 页观测面（P5.2）**：按 `V`（扫描码 `0x2F`）进入后，`V` 页由 `app/user/page/page.c` 唯一持有页面模式、选中 PCB 槽和 10 行滑动窗口；`app/arm/app/app.c` 在 `user_live` 时把扫描码交给 `user_page_key()`，不再散落翻页分支。`Tab` 循环换页，数字 `1..4` 直达 `SLOTS/TREE/ADDRESS/FD+SIG`，`Up/Down` 移动选中槽，`PgUp/PgDn` 整窗滚动，`Home/End` 跳首尾；`keyboard_poll()` 的 `0xE0` 前缀由 page 模块吞掉，方向键要下一帧才生效。渲染拆成 `render.c`（公共标题/页签/slot 状态/底栏/分派）、`draw.c`（统一的十进制/十六进制与状态名颜色）、`slots.c`、`tree.c`、`address.c`、`fd.c`，每个页面只负责自己的行布局，不再复制格式化代码。
- **只读观测 API（P5.2）**：`include/kernel/proc/fd.h` 增加 `struct proc_fd_info { int used; u32 kind; }` 与 `proc_fd_snapshot()`，逐 fd 返回 kind，不暴露 pipe 指针；`include/kernel/proc.h` 增加 `process_slot_space()`、`process_slot_fd_info()`，由 PCB 槽直接取 AS/fd，FREE 槽返回空；`process_slot_used()` 只数 LIVE/ZOMBIE，P10 后回收槽直接 FREE，`dead` 固定为 0。
- **100 轮 fork/wait 压测（P5.3）**：按 `S`（扫描码 `0x1F`）复用 P5.1 池页外壳进入 `AEVUM / P5.3 FORK/WAIT STRESS`。父进程 `stressA` 连续 100 轮 `fork`，子进程执行 `exit(7)`，父进程每轮 `wait` 并校验返回 PID 等于本轮子 PID、status 等于 7；首轮子 PID 记入 `0x401100`，后续 99 轮必须复用同一 PID。全部通过后父进程写 `0x4010C4` 的 `POOL` 魔数并保持存活供观察，按 `S` 停止时 `exit(18)`。页面复用 `0x4010C0` 作为轮次、`0x401100/0x401104` 作为首/末 PID，不新增独立页，也不复制池统计：`round 100/100 first 5 -> 5 live 1 state STRESS OK`，第二行显示 `PHYS 6/255 proc 1/12 AS 1/12 KSTK 1/12`。两轮独立 `S` 实测均为 `PID 5`、`PHYS 6/255`、`proc/AS/KSTK 1/12`，ASID 从 1 换到 102 后继续复用；证明 100 次子进程回收后 PID、地址空间槽、内核栈槽与物理页都回到单父进程基线。
- **P11.1 定时阻塞（sleep / wait_timeout）**：`task_struct` 新增 `wake_tick`；`task_block_until_timeout(ready,ctx,ticks)` 进入时固定绝对 tick deadline，PIT 每 tick 扫描 BLOCKED 任务，到期改 READY。用户 ABI：`eax=24 sleep(ebx=ticks)` 返回 0/-1；`eax=25 wait_timeout(ebx=pid,ecx=user_status,edx=ticks)` 返回子 PID/0 超时/-1，`ticks=0` 只做一次非阻塞查询。D lane 慢子 `sleep(20)` 后 exit(7)，父 `wait_timeout(5)` 必须超时返回 0、再 `wait_timeout(50)` 拿到 PID/7；快子 `sleep(1)` 后 exit(9)，父 `wait_timeout(20)` 拿到 PID/9，全部通过写 `0x401250="TMR1"`，V 页 ADDRESS 显示 `tmr OK`。C 页底部显示 `SLEEP b <blocks> w <wakes> tmo <timeouts>`；实测 `b 2 w 2 tmo 3`。PIT 100Hz 下 1 tick=10ms，超时不是高精度定时器。
- **P11.2 优先级生命周期（setpriority / getpriority）**：`eax=26 getpriority(ebx=tid)` 返回 0..3/-1，`eax=27 setpriority(ebx=tid,ecx=priority)` 返回 0/-1；`tid=0` 表示当前任务，0 最高、3 最低。`task_priority_set()` 只改后续 PIT 记账权重，不重算历史 `vruntime`；ZOMBIE 不接受 set，用户态只能操作 user 任务，不能改 control/render/idle。fork 子进程从父任务继承优先级，clone 线程从当前线程继承，`process_clone_thread_current()` 删除了原先被忽略的 priority 参数。D lane 验证默认 1、set 3、非法 4 返回 -1、内核任务拒绝、fork 子为 3、clone 线程为 2；全部通过写 `0x401260="PRI1"`，V 页 ADDRESS 显示 `TASK READY P1` 与 `pri OK`。ring3 code 压到 4,091B，距 4KB 上限只剩 5B。
- **P11.3 对象等待队列与阻塞 I/O（read_block / write_block）**：`eax=15 read` / `eax=16 write` 的旧非阻塞语义完全不变；新增 `eax=28 read_block(ebx=fd, ecx=user_buf, edx=len, esi=ticks)` 与 `eax=29 write_block`，`ticks=0` 等价非阻塞，`ticks>0` 最多等待 PIT tick；成功返回字节数、pipe EOF 0、参数/端点错误 -1、超时 -2。`task_struct` 增加 `waitq` 指针，`task_block_on()` 在 `cli` 窗口内先重查条件再查 deadline，才置 BLOCKED；`wait_queue_wake_all()` 广播 16 槽等待者，只改 READY 不直接切任务。pipe 增加 `read_wq/write_wq`，空读且写端活着时等、容量不足且读者存在时等，读写成功和端点关闭分别广播；console 输入队列增加 `input_wq`。`proc_fd_read_block/write_block` 做用户范围校验与 `copyin/copyout`，用户指针不直接进 pipe 缓冲。验收新增独立 image 3 `user/exec-io/exec_io.asm`（644B，固定 `0x400000`）：空 pipe `read_block(...,2)` 必须返回 -2；fork 子 `sleep(20)` 写 `IORD`、父 `read_block(...,500)` 读回 4B；fork 后父保留读端填满 64B、`write_block(...,1,500)` 返回 1。D lane 用 `spawn(image_id=3)` 启动并 wait，成功写 `0x4012A0="IOB1"`、`0x401040="IO13"`，V 页 ADDRESS 显示 `io OK`、`REAP1 6 img3 a0 e0`。C 页新增 `IO b <blocks> w <wakes> tmo <timeouts>`；最终实测 `IO b 3 w 2 tmo 1`、`SLEEP b 4 w 4 tmo 6`。
- **P11.4 idle 停机与空闲调度**：`task_pick_next()` 不再把 idle 当普通 READY 参与 `vruntime` 竞争；只要存在非 idle READY 就返回该任务，只有全系统没有非 idle READY 时才返回 `idle_best`。`arm_app_init()` 在 idle 创建后调用 `task_set_idle_task()` 登记唯一 idle；`proc_idle()` 被选中后执行 `task_note_idle_hlt(); sti; hlt`，不再忙等 `task_yield()`。主页面 I 键（扫描码 `0x17`）让 control/render 主动 `task_block()` 40 tick，制造“无其它 READY”的探针窗口；idle 到期后唤醒两者、清 phase 再 `task_yield()`。C 页第 21 行显示 `IDLE hlt <count>`，正常机械臂负载下为 0，探针后实测 39；探针结束继续运行到 PIT 8,043 tick 仍为 39，证明 idle 只在没有非 idle READY 时被选中。K 键 `task_block()/task_wake()` 调试路径保留；B 键通过 `task_force_idle_demo()` 强制选中 idle 一次。新增代码把 BSS 推到 `0x981FC`，超过旧 `0x98000`，因此 boot 从 192 扇区扩到 208、`AEVUM_KERNEL_END` 同步到 `0x9A000`；clean `kernel.bin=82,312`，BSS 余量 3,588B。
- **P11.5 显式时间片与 PIT 延迟重调度**：`task_struct` 增加 `slice_left`，`TASK_SLICE_TICKS=4`（100Hz 下 40ms）；boot、创建、`task_pick_next()` 选中任务时都重置整片。IRQ0 每 tick 仍做 `task_wake_expired()`、`cpu_ticks++`、`vruntime += 64/weight`，然后递减 `slice_left`；片未尽直接返回当前 `irq_frame`，不调用 `task_pick_next()`，片到 0 才重选。`task_yield()` 视为放弃剩余片，所以协作式 control/render 正常负载下 `EXP` 为 0；按 `T` 的 CPU-bound duel 不让出，C 页从 `PREEMPTIONS 0/EXP 0` 增长到 100/100，证明每 4 tick 重选一次。C 页第 19 行新增 `QUANTUM 4t` 与 `EXP <n>`，第 24 行解释；`EXP` 是片到期次数，`PREEMPTIONS` 是实际换任务次数，idle 探针后为 109 对 100。clean `kernel.bin=82,664`，`sizeof(task_struct)=116B`，BSS 余量 7,268B。
- **P11.6 等待队列 ticket FIFO 与定向唤醒**：`struct wait_queue` 改为 `u32 next_ticket + u32 wakes`，`task_struct` 增加 `wait_ticket`；旧 `eax=15/16/28/29` 的返回值和参数完全不变。首次进入对象阻塞取单调票据，`wait_queue_wake_one()` 扫 16 槽选择票据最小的 BLOCKED 等待者置 READY；条件重查失败再次阻塞时保留原票据，不会被排到队尾。pipe 读消费后定向唤醒写者、仍有数据再唤醒下一个读者，写入后只唤醒一个读者；console 输入同样只唤醒一个读者。关闭/EOF 仍用 `wake_all`，保证所有读者看到 EOF。C 页第 23 行新增 `WQ blk <n> one <n> all <n> dep <n>`；image 3 新增三个延迟 20/40/60 tick 的等待者，父按 A/B/C 放行后必须按 `1/2/3` 回写 id。实测 `WQ blk 9 one 8 all 0 dep 3`、P5.3 `STRESS OK`、V 页 `img 3 a0 e0 / io OK`；clean `kernel.bin=84,264`，`sizeof(task_struct)=120B`，BSS 余量 5,508B。边界：FIFO 只覆盖已阻塞者，新到者可以插队；不做 EINTR、poll/select、等待延迟统计、per-object 链表或 SMP（EINTR 已由 P11.7 补上）。
- **P11.7 可中断阻塞（EINTR）**：`read_block`/`write_block`/`sleep`/`wait`/`wait_timeout`/`thread_join` 在阻塞期间遇到当前进程 pending signal 时统一返回 -3（EINTR）；旧成功返回值和 -1/-2 错误码不变。`process_signal_take_current()` 按信号号取走最低位 pending 并清 bit，避免同一信号让每次重试都立刻 EINTR；`task_block_until/until_timeout/on` 返回内部码 2，条件满足仍优先，同一 tick 的 deadline 先于信号。syscall case 22 单独保留 -3，不压成 -1。C 页第 22 行新增 `EINTR <n>`；image 3 追加 sleep、空 pipe read_block、满 pipe write_block、wait_timeout 长子、thread_join worker 五段 SIGCHLD 打断测试，失败码 20..25。实测 `EINTR 7`、`SLEEP b 18 w 14 tmo 16`、`IO b 11 w 8 tmo 1`、`WQ blk 11 one 8 all 0 dep 3`、P5.3 `STRESS OK`、V 页 `img 3 a0 e0` 且 `SIG pending 0`；clean `kernel.bin=85,640`，`sizeof(task_struct)=120B`，BSS 余量 4,132B。边界：pending 是进程级，不新增 `kill()`、异步 handler、`SA_RESTART`、per-thread pending、poll/select 或 SMP；只验收 SIGCHLD。
- **P11.8 kill 与异步信号投递**：新增 `eax=30 kill(pid,sig)` 与 `eax=31 sigaction_flags(sig,handler,mask,flags)`；旧 `eax=11 sigaction` 等价 flags=0，15/16/28/29 的旧返回值不变。`kill` 只允许 self 或直接子进程，`sig=0` 只查存在，拒绝 SIGSTOP；目标 LIVE 时置 pending 并唤醒阻塞线程。P11.7 的阻塞点改为 peek，真正消费在返回用户态前的 `process_signal_deliver_async()`：IGN 直接消费，SIGCHLD 的 DFL 忽略，其它 DFL 终止，用户 handler 复用 signal frame，一次投递一个。`process_signal_restart_prepare()` 在阻塞 syscall 返回 -3 且最低位 pending 有用户 handler + `SA_RESTART` 时，确认 `eip-2` 是 `CD 80` 后回退 `eip` 并把 `eax` 还原成原 syscall 号；sigreturn 后重执行。`loader.asm` 在 int80 和 PIT/task 返回用户态前各投递一次。image 3 追加 SIGINT-EINTR、SA_RESTART、SIGTERM 默认终止、CPU-bound 异步 handler 四段，失败码 26..29，成功输出 `P11.8 KILL OK`。实测 `REAP1 6 img 3 a0 e0`、`SIG pending 0`、`EINTR 7`、P5.3 `100/100 STRESS OK`；clean `kernel.bin=87,080`，BSS 余量 2,692B、镜像余量 19,416B。边界：没有 uid/gid、进程组、作业控制、信号排队、per-thread pending、`SA_SIGINFO`/`SA_NODEFER` 或用户 `sigprocmask`；`SA_RESTART` 只覆盖 `int 0x80` 返回点。
- **构建裁剪（P5.3）**：所有 C 目标加 `-ffunction-sections -fdata-sections`，链接加 `--gc-sections`，只保留从 `start` 可达的函数/数据。P5.2 单体链接里有约 1.3KB 不可达代码；P5.3 新增代码后仍把 clean `kernel.bin` 控制在 64,808 字节。P6.0 收口后 clean `kernel.bin=64,488`，余量 1,048 字节，BSS 结束于 `0x9387C`。手动关节模式与 19 字段状态帧加入后 clean `kernel.bin=66,248`；2–8 关节通用身体描述、动态状态帧和通用 VGA 加入后 clean `kernel.bin=68,264`，上限提高到 98,304。
- **P6.0 细节收口**：`process_wait()` 的 `pid>0` 分支先校验当前进程的直接子进程，非子进程或不存在立即返回 -1；`pid<=0` 等任意直接子进程，`waitpid(0)` 不再误等 PID 0。父进程退出/异常终止时，`proc_orphan_children()` 同时重挂 LIVE 和 ZOMBIE 直接子进程，并记录 `from/to/children[]` 最近事件；V 页 TREE 第 20/21 行显示 `ORPHAN` 与 `kids`，G 页 reparent 面板显示 4 个子进程，超过 4 个时显示 `+N more`。速度基线见 `PERFORMANCE.md`。
- **进程地址空间 / COW fork / VMA / 物理页池**：按 `V`（扫描码 `0x2F`）启动五个用户进程：`userA` / `userB` 做独立地址空间计数、`brk` 扩堆与 heap `peek/poke`，`userC` 做用户态 `#PF` 样本，`userD` 做四轮 fork/wait，`userF` 直接写 guard。每个进程有自己的页目录和 CR3，PD/PT/code/data/stack/control 全部从 `0x100000..0x1FEFFF` 的 255 页位图池分配，每页带引用计数；用户 VA 固定为 code `0x400000`、data `0x401000`、guard `0x402000`（VMA 存在但无 PTE）、stack `0x403000`、control `0x404000`、heap `0x405000..0x410000`。全屏页显示 `PID PPID ST EXIT CR3 AS PGS/C BRK COMM`、6 个 VMA 的地址/权限、`brk/peek/poke/guard` 结果、COW 统计、`PHYS PAGES used/total`、`pipe obj`，另有 `int 0x80`、fork/wait、#PF、#DE 与 SIGCHLD 面板。D 的四次 fork 都返回 8；第一轮子进程写 `CHLD` 后 `exit(7)`，父 wait 得到 status 7 并显示 `WAIT OK 7 / COW OK`；第二轮子进程先 `sigaction(SIGFPE, handler, 0)`，再 `1/0` 触发 `#DE`，signal frame 进入 handler、`sigreturn` 返回除零后的指令，写 `HAND` 后 `in al,0x60` 触发 `#GP -> SIGSEGV(11)`，父 wait 得到 status `0x8000000B` 并显示 `HAND OK`；第三轮子进程 `exec(1, argv, envp)`，参数为 `alpha/beta/NULL` 与 `MODE=p42/LEVEL=2/NULL`；新镜像从新栈校验四串后写 `ARGV/ENVP` 并 `exit(10)`，父 wait 得到 10 并显示 `P4 exec 8 wait 8 st 0000000A img 1 EXEC OK arg 2 env 2`。第四轮父 `pipe()` 得到 fd 2 读 / fd 3 写，fork 后父关写端、子关读端并 `exec(2)`；镜像在 exec 后仍通过 fd 3 写 `PIPE` 并 `exit(12)`，父 wait 得到 12 后从 fd 2 读回 4 字节，校验 `0x45505049`，关闭读端，显示 `P4.3 pipe 8 wait 8 st 0000000C fd 2>3 r4 PIPE OK`。
- **COW fork**：`address_space_clone()` 只分配新 PD/PT，把 data/stack/heap 以及内核可写的 control 页清 `RW`、置 PTE bit 9 后父子共享，代码页只读共享；物理页引用计数加一。已经是 COW 的页第二次 fork 时必须保留 COW 位，否则会把可写页退化成只读页；`address_space_clone()` 现在保留 `PAGE_COW`，D 第二轮 fork 的栈页因此仍可写。首次写触发 `#PF` 时，`address_space_handle_user_fault()` 识别 COW 位，分配新页、复制内容、恢复 VMA 权限并返回原帧重试；`copyout` 命中 COW 页也先拆分再写。V 页实测 `forks 4 shared 16 live 2 breaks 7 last AS4 old 0011C000 -> 0011B000`，其中 signal frame 写入用户栈也触发了一次 COW 拆分。
- **VMA 与 copyin/copyout**：`eax=8 brk(ebx)` 只允许 `[0x405000,0x410000]`，增长时零填新页并把 heap VMA 的 end 推到新 brk；`eax=9 peek(ebx)`、`eax=10 poke(ebx,ecx)` 逐字节翻译用户 VA，按 VMA 权限 + PTE present/user/RW/COW 校验，不再让内核直接解引用用户指针。V 页实测 A/B：`brk 0x406000`、heap `0x405800` 写入 `HELP` 后 `peek=0x48454C50`、越界 `brk(0x500000)` 返回 `0xFFFFFFFF`、`poke(guard 0x402000)` 返回 0。
- **`#PF` 故障隔离**：`userC` 在私有数据页置 `mode=1`，数到 5000 后故意写内核显存 `0xB8000`；`userF` 在 `mode=3` 直接写 guard `0x402000`。CPU 触发 `#PF` 后，heap VMA 内的缺页由 `address_space_handle_user_fault()` 零填并 `iret` 重试，COW 写页由同一条路径拆成私有页；guard/RO/非法地址以 `process_terminate_current(SIGSEGV)` 终止当前进程，`userA/userB` 的 CR3 与计数器继续推进。全屏页显示 `P/W/U`、`count` 和 fault 的 `pid/eip/cr2/err`，C/F 行分别显示 `FAULT` / `GUARD` 与 `SIG11`。
- **物理页池**：`address_space.c` 用 255 位位图 + 每页引用计数统一分配/释放 PD、PT 和用户页；`address_space_release()` 遍历 PT 逐页减引用，`fork` 不再复制用户页，只在首次写时按 COW 拆分。V 页 `PHYS PAGES 20/255` 在停止/重启两轮后保持一致，证明页、AS 槽、PID 和内核栈都没有泄漏。
- **内核态 `#PF` 诊断页**：按 `K`（扫描码 `0x25`）从 `render` 的 kernel task 写 `0x00400000`，越过 0–4 MB 恒等映射，CPL0 触发 `#PF`。页面进入即 `cli`，显示 `task 2 render`、`CS=0x0008 / CPL 0`、`EIP`、`CR2`、`ERR`、`CR3`、`P/W/U/RSVD/I-D`、fault count、recoveries 和 `kernel image 0x00080000..0x00090800`。按 `R` 临时建立 PDE 1（页表 `0x13000`、诊断页 `0x14000`），让出错写指令执行成功并通过原异常帧 `iret` 返回，随后撤销映射、重新武装 `K`；按 `H` 或遇到非探针内核 fault 仍停机。
- **显示输出**：直接写显存 `0xB8000`（80×25 文本模式），`render` 每帧在影子显存 `0x20000`
  画三自由度机械臂、目标点、轨迹和右侧数值面板，再整块搬到 VGA；同一帧通过 COM1
  把变化的文本单元发给 QEMU 串口，供桌面「内核实况」窗口实时显示。
- **抢占决斗**：按 `T`（扫描码 `0x14`）创建 `spinA` / `spinB` 两个 CPU 密集型任务，循环里都不调用
  `task_yield()`；全屏显示两条 lane 的实时步数、进度条和 80 格 IRQ0 抢占磁带。只有 PIT 真的抢占 CPU，
  两条 lane 才会同时推进，磁带才会出现青/洋红交替的条纹；再按 `T` 停止并回到机械臂画面。
- **优先级与 CPU 统计**：`priority 0..3` 映射为权重 `8..1`；每个 PIT tick 给当前任务 `vruntime += 64 / weight`，`task_pick_next()` 选择 READY 任务中 `vruntime` 最小者，同值按 `prev` 之后的槽位轮转。P11.9 用 64 位 `min_vruntime` 钳住睡眠唤醒，防止长睡眠任务靠过期小 `vruntime` 一次抢回整段 CPU；权重全为正，低优先级任务仍会轮到，不会被 FIFO 式饿死。每个任务累计 `cpu_ticks`，面板第二行显示真实 CPU 百分比；按 `C` 打开全屏 `AEVUM / CPU TIME ACCOUNTING`，列出 PID、TASK、PRI、WT、STATE、CPU%、TICKS、SHARE，底部是 PIT TICKS、SWITCHES、PREEMPTIONS，以及 P11.1 `SLEEP` 与 P11.3 `IO` 两组阻塞统计。P11.2 增加 `eax=26/27` 运行时读写优先级，fork/clone 继承，V 页 `TASK` 行显示 `P<n>`；P11.3 增加 `eax=28/29` 带 tick 上限的阻塞 I/O；P11.4 让 idle 只在没有非 idle READY 时运行，C 页新增 `IDLE hlt`，正常负载下 idle 的 CPU% 为 0，I 键探针后才会增加；P11.5 把每 tick 重选改成 4 tick 显式片，C 页新增 `QUANTUM 4t` 与 `EXP`，协作负载 `EXP=0`，CPU-bound duel 每 4 tick 抢占一次；P11.6 把对象等待队列改成 ticket FIFO + 定向唤醒，C 页新增 `WQ blk/one/all/dep`，常规数据到达只叫醒一个等待者；P11.7 把 pending signal 接入阻塞判断，新增 `EINTR -3`；P11.8 新增 `kill`/`sigaction_flags` 与返回用户态前的异步投递，`SA_RESTART` 只覆盖用户 handler + `int 0x80` 返回点。P13.1 把 RT 任务从 CFS-lite 的 `vruntime` 扫描里拆出，改由 4 级位图 + 每优先级双向链表做 O(1) 选队头；RT 同优先级 FIFO、高优先级抢占低优先级，fair 任务仍走原 `vruntime` 规则，idle 规则不变。RT 不再消耗 4 tick 固定片，一直跑到 block/yield、预算耗尽或被更高 RT 抢占；普通构建没有 RT 任务，位图恒为 0。按 `Q`（扫描码 `0x10`）打开 `AEVUM // REAL-TIME READY QUEUE`：双线框仪表盘，左侧 4 级队列 `LANE / BIT / READY / HEAD / TAIL / LOAD`，右侧 `FIFO BURST` 两张任务卡显示 `qrtA #1`、`qrtB #2`，中部 `DISPATCH TRACE` 画出 `[P1 A #1] ==> [P1 B #2] ==> [HEAD] ==> [RUN] ==> [EXIT]`，底部是 4 级位图、`QUEUED/16`、`ERR`、`SWITCHES` 脉冲条和 `FIFO SLOTS`；由两个 priority 1 动态 kthread 现场做一次同优先级 FIFO 顺序探针，再按 `Q` 返回。普通构建没有常驻 RT 任务，截图时活队列已排空，`FIFO BURST` / `DISPATCH TRACE` / `FIFO SLOTS` 保留 A=1、B=2 的突发证据。P13.2 在 P13.1 的 RT 队列上接入 LAPIC timer + HPET 参考：RT 预算活动期以 1ms one-shot 伺服，第 2ms 事件强制摘链并置 `need_resched`；PIT 仍负责 100Hz 逻辑 tick，fair 仍用 4 tick 片。Q 页底部新增 `TIMER LAPIC 1MS / HPET REF`、`SERVO`、`HARD` 三列，并按 `Q` 返回主画面。P13.3 把截断从“单次通过”升级为可回归契约：`RT_STAT` 拆出 `preempt=1000 cut_n=1000 cut_us=2000 post_max_us=9100 post_avg_us=9099 after_max_us=999 after_avg_us=999`，`after_*` 只计恢复后实际执行的 TSC，`post_*` 是从截断到作业返回的墙钟（含 throttle 等待）；作业在 2ms 被截断后于下一个 tick 恢复，补跑剩余 1ms，`miss/exec_wall_over` 仍按 3ms 工作对 10ms deadline 的墙钟口径为 1000，不代表截断失败。bg=2/4/8 的 `-icount` 矩阵均为 `hard=1000 cut_n=1000 after_avg≈999`，Q 页 row19 改为 `TIMER LAPIC 1MS / CUT <us> / SERVO / HARD`。P13.4 把工作降到 1.5ms（预算 2ms）做可调度基线：bg=2/4/8 都是 `miss=0 hard=0 cut_n=0`，`lat_max_us=7/7/8`、`exec_max_us=1500`；再用 2.1ms 边界得到 `cut_us=2000 after_avg=99`，证明只在超预算时截断并补跑约 0.1ms。同口径 Linux i386（HZ=100、无 high-res、PREEMPT_NONE）的 1.5ms 周期唤醒 `lat_p99≈9.4~9.8ms`、`miss=1000`，Aevum 在同一矩阵为 `p99<1µs`、`max 7~8µs`、零 miss；Linux 不是 PREEMPT_RT，这组数只说明普通 tick 内核的粒度，不是 Linux 硬实时上限。
- **P13.5 RT 互斥锁与优先级继承**：新增 `kernel/sync/rt_mutex.c` 与 `include/kernel/rt_mutex.h`，提供 `rt_mutex_init/lock/unlock`；`task_struct` 增加 `base_priority`、`pi_priority`、`pi_boosted`，`priority` 改为有效优先级。等待者拿锁时把 RT owner 提升到等待者中的最高优先级，解锁后恢复基准值；只支持单层持锁，不递归。普通构建 `kernel.bin=99,048B`、`text=98,795`、`data=232`、`bss=18,044`、`__bss_end=0x9C97C`，BSS 余 1,668B。`RT_PI_TEST` 夹具：RT 3 持锁后阻塞，RT 1 CPU-bound 3 tick，RT 0 再拿锁。3 次结果：无 PI `wait_us=36,303/38,406/37,993`、`med_ticks=3`、`boost=0`；有 PI `wait_us=7,552/7,945/7,858`、`med_ticks=0`、`boost=restore=1`，锁后 `low_p` 恢复 3。PI 把等待压到约 1/4.8，且中优先级任务一次都没抢到 CPU；`wait_us` 含 QEMU 调度和 TSC 开销，不是纯临界区 1.5ms。两种构建的主循环仍 `miss=0 qerr=0`。
- **P13.6 RT 互斥锁的立即优先级天花板**：`struct rt_mutex` 增加 `ceiling`，`rt_mutex_init(m, pi, ceiling)` 支持无继承、等待者 PI、立即天花板三种模式。天花板模式下 owner 拿锁当下就提升到静态 ceiling，不等高优先级任务来等锁；解锁先恢复 `base_priority` 再唤醒，修掉 P13.5 的 wake/restore 窗口。`RT_PI_MODE=0/1/2` 夹具 3 次：无继承 `wait_us=39,061/33,239/40,162`、`lock_p=3 med_ticks=3`；等待者 PI `7,661/9,856/11,519`、`lock_p=3 med_ticks=0`；立即天花板 `7,078/9,083/8,374`、`lock_p=0 med_ticks=0`。普通构建体积与 P13.5 相同：`kernel.bin=99,048B`、`bss=18,044`、BSS 余 1,668B；三种主循环均 `miss=0 qerr=0`。
- **P13.7 RT 互斥锁的多锁嵌套 PI 链**：`task_struct` 增加固定 2 层 `held_mutex[]`、`held_mutex_depth` 和 `pi_blocked_on`；`task_rt_mutex_boost_chain()` 先提升 owner，再沿 `owner->pi_blocked_on` 提升内层 owner。`rt_mutex_lock()` 无 owner 时检查深度 `<2` 并压栈，有 owner 时写 `blocked_on` 后链式提升；`rt_mutex_unlock()` 按 LIFO 出栈，只有最外层解锁才 restore。`RT_PI_CHAIN` 夹具：`pimed`(RT2) 拿 B 后阻塞，`pilow`(RT3) 拿 A 后阻塞 B，`pburn`(RT1) 干扰，`pchainhi`(RT0) 等 A。各 3 次：chain=0 `wait_us=23,656/24,259/24,099`、`burn_ticks=3 med_p=2`；chain=1 `3,606/3,402/3,444`、`burn_ticks=0 med_p=0`。普通构建 `kernel.bin=99,208B`、`bss=18,300`、BSS 余 1,252B；6 次主循环均 `miss=0 qerr=0`。
- **P13.8 全量回归与两层锁压力**：普通镜像重跑 P5.3、P11.x、P12.1 与 P13.3/P13.4/P13.6；`RT_LOCK_STRESS` 做 1000 轮 `A -> B` / `B -> A`，3 次均 `err=0 third=-1 lifo=-1 depth=0`。P5.3 两轮 `100/100 STRESS OK`；P13.3 `cut_n=1000 cut_us=2000 after_avg=999`；P13.4 1.5ms 零截断、2.1ms `hard=1000 after_avg=99`；P13.6 三模式均 `rc=0`。**P11.7 失败：image3 连续 6 次 exit 25，`thread_join` 期望 -3 实得 0；P11.8/P12.1 未执行。** 普通构建体积与 P13.7 相同 `kernel.bin=99,208B`、BSS 余 1,252B。
- **无 libc**：完全 freestanding，字符串输出、整数转十进制和 Q16.16 定点运算都由内核自己实现。

### 操作按键总表

所有按键都由 `app/arm/app/app.c` 的 `proc_render()` 轮询 PS/2 扫描码分发。下表是完整集合，没有隐藏的组合键。

| 当前画面 | 按键 | 扫描码 | 动作 | 返回与限制 |
| --- | --- | --- | --- | --- |
| 机械臂主画面 | `空格` | `0x39` | 切换零空间（nullspace）开/关 | 只在主画面生效；不按键也会每 420 帧自动切换一次 |
| 机械臂主画面 | `P` | `0x19` | 创建 `probe` 任务，全屏显示 PID、生命周期和 PIT 抢占计数 | 自动演示 6000 帧后任务结束并回主画面；没有手动停止键，探针显示期间按 `P` 不会重入 |
| 机械臂主画面 | `B` | `0x30` | 让 `idle` 执行 `task_block()`；再按一次 `task_wake()` 唤醒 | 只在主画面生效；阻塞时第三行显示 `3id B`，并出现 `BLOCKED` 提示 |
| 机械臂主画面 | `I` | `0x17` | P11.4 idle 探针：control/render 主动 block 40 tick，idle 成为唯一 READY 并 `sti; hlt` | 只在主画面、无 demo、idle 未 block 时生效；探针结束后自动唤醒两者；C 页查看 `IDLE hlt` |
| 机械臂主画面 | `T` | `0x14` | 进入/停止 `PREEMPTION DUEL`，创建 `spinA` / `spinB` 两个不让出的 CPU 密集型任务 | 只在主画面启动；停止后等两个 lane 任务结束再回主画面 |
| 机械臂主画面 | `C` | `0x2E` | 进入/退出全屏 `AEVUM / CPU TIME ACCOUNTING` | 只在主画面启动；在统计页再按 `C` 返回；底部有 PIT TICKS / SWITCHES / PREEMPTIONS / QUANTUM / EXP |
| 机械臂主画面 | `Q` | `0x10` | 进入/退出全屏 `AEVUM // REAL-TIME READY QUEUE`，双线框仪表盘显示 4 级 RT 队列表、`FIFO BURST` 任务卡、`DISPATCH TRACE`、位图和 `SWITCHES` 脉冲条；底行显示 `TIMER LAPIC 1MS`、`CUT <us>`、`SERVO`、`HARD` | 只在主画面启动；在队列页再按 `Q` 返回；普通构建的 PICK 采样为 0，`CUT` 为 0 |
| 机械臂主画面 | `F` | `0x21` | 进入/退出全屏 `AEVUM / FPU CONTEXT LAB` | 只在主画面启动；在实验页再按 `F` 返回 |
| 机械臂主画面 | `G` | `0x22` | 进入/退出全屏 `AEVUM / PROCESS FAMILY TREE` | 只在主画面启动；在树页再按 `G`，等 `famP` / `famA` / `famB` 都结束后回主画面 |
| 机械臂主画面 | `Y` | `0x15` | 进入/退出 `AEVUM / P5.1 SLOT POOL`，根进程 fork 8 个子进程并全部保持存活 | 只在主画面启动；在池页再按 `Y`，等 9 个进程都结束后回主画面 |
| 机械臂主画面 | `S` | `0x1F` | 进入/退出 `AEVUM / P5.3 FORK/WAIT STRESS`，父进程 100 轮 fork/wait | 只在主画面启动；在压测页再按 `S` 停止父进程并回主画面 |
| 机械臂主画面 | `V` | `0x2F` | 进入/退出全屏 `AEVUM / V PAGE OBSERVABILITY / P5.2`，启动 A/B/C/D/F 五个用户进程，D 做四轮 fork/wait（第三轮 exec、第四轮 pipe） | 只在主画面启动；在页内再按 `V` **立即回主画面**，demo 进程由 `user_demo_stop()` 异步收尾，不再等 A/B/D 结束 |
| `V` 页 | `Tab` | `0x0F` | 循环切换 `1 SLOT / 2 TREE / 3 AS / 4 FD/SIG` 四页 | 由 `user_page_key()` 消费 |
| `V` 页 | `1`..`4` | `0x02`..`0x05` | 直达 SLOTS / TREE / ADDRESS / FD+SIG | 数字键比 Tab 稳定，取证时优先用 |
| `V` 页 | `Up` / `Down` | `0x48` / `0x50` | 选中槽减/加 1，选中槽超出 10 行窗口时窗口跟随 | 方向键的 `0xE0` 前缀由 page 模块吞掉，下一帧实际码才生效 |
| `V` 页 | `PgUp` / `PgDn` | `0x49` / `0x51` | 整窗上/下滚 10 槽，选中槽同步移动 | 12 槽只有 `1-10/12` 与 `3-12/12` 两个窗口 |
| `V` 页 | `Home` / `End` | `0x47` / `0x4F` | 跳到槽 0 / 槽 11 | 空槽仍显示池统计与 `EMPTY` 提示 |
| 机械臂主画面 | `K` | `0x25` | 触发内核态 `#PF`，进入全屏 `AEVUM / KERNEL PAGE FAULT` | 受控探针；页内按 `R` 恢复并重新武装，按 `H` 停机；不参与其他演示 |
| `KERNEL PAGE FAULT` | `R` | `0x13` | 临时映射 4 MB 诊断页，执行完出错写指令并 `iret` 返回主画面 | 只对 `K` 探针可用；返回后撤销 PDE 1，下一次 `K` 会重新触发 |
| `KERNEL PAGE FAULT` | `H` | `0x23` | 立即停机 | 保留现场，不返回；非探针内核 fault 没有这个选择 |
| `PREEMPTION DUEL` | `T` | `0x14` | 停止决斗 | 全屏页只响应 `T`；停止不是立即切页，要等两个任务退出 |
| `CPU TIME ACCOUNTING` | `C` | `0x2E` | 返回机械臂 | 全屏页只响应 `C` |
| `REAL-TIME READY QUEUE` | `Q` | `0x10` | 返回机械臂 | 全屏页只响应 `Q`；探针任务已退出，页面保留 `FIFO BURST` A/B 卡与轨迹 |
| `FPU CONTEXT LAB` | `F` | `0x21` | 停止实验并返回机械臂 | 全屏页只响应 `F` |
| `PROCESS FAMILY TREE` | `G` | `0x22` | 停止演示并返回机械臂 | 全屏页只响应 `G`；有任务未退完时继续显示树页 |
| `COW FORK + SIGNALS + PHYS` | `V` | `0x2F` | 停止所有用户进程并返回机械臂 | 全屏页只响应 `V`；**UI 立即回主画面**，demo 进程异步收尾 |
| `PID PROBE` | 无 | — | 自动运行到结束 | 没有停止键，也不能在探针页启动其他演示 |

补充：

- 这些全屏演示互斥：`T` / `C` / `Q` / `F` / `G` / `V` 只能从机械臂主画面进入，进入后其他演示键不响应。`K` 是受控故障注入入口，不参与互斥；页内 `R` 恢复，`H` 停机。
- 机械臂主画面没有方向键、目标点手动输入、`ESC` 退出或 `R` 复位；目标点沿预设轨迹自动移动，`make run` 的退出由 QEMU 窗口或宿主终端负责。
- `P` / `B` / 空格只在机械臂主画面生效；任何全屏演示期间按它们都不会改变后台任务。
- **`B` 不是一页**，它只是 idle 任务的 `BLOCKED ↔ READY` 演示键：按一次让 idle 主动 `task_block()`，再按一次 `task_wake()`。P11.4 之后 idle 平时不会被调度，所以 B 键会让调度器强制选中 idle 一次，确保 `task_block()` 真的执行；阻塞后画面出现 `BLOCKED: idle waiting task_wake()`，调度行显示 `3id B 0%`。
- `V` 的用户任务使用独立 CR3；`V` 页显示的 `CS=0x1B` 是最近一次 `int 0x80` 帧里的真实 CS，不是固定字符串。

### 运行效果

```
 3-DOF Arm | PIT 100Hz | P/B/T/C/F/G/V/K
 ARM STATUS                         nullsp ON
 joint angles
 q1  -37.1  q2  60.9  q3  83.6
 ...
 tip @  elbow O  wrist *  target o  path .  base #  radial sweep
```

机械臂会持续追踪右侧面板里的目标点，`control` 算控制量，`render` 负责渲染；PIT 100Hz 抢占保证 CPU 密集型任务也不能独占。

---

## 二、环境安装与要求

### 运行环境

- 64 位 Linux（Debian / Ubuntu / Kali 等），或 Windows 上的 WSL2。
- 终端里能执行 `make`。

### 依赖工具

| 工具 | 用途 | 说明 |
| --- | --- | --- |
| `nasm` | 汇编器 | 汇编 `boot.asm`、`loader.asm` |
| `gcc` | C 编译器 | 需要支持 `-m32` 编译（能生成 32 位目标文件即可） |
| `ld`（binutils） | 链接器 | 用 `-m elf_i386` 按 `kernel.ld` 链接 |
| `make` | 构建工具 | 执行 Makefile |
| `qemu-system-i386` | 模拟器 | 运行生成的 `os.img` |

### 安装命令（Debian / Ubuntu / Kali）

```bash
sudo apt-get update
sudo apt-get install -y nasm gcc make binutils qemu-system-x86

# 可选：想在系统里另外链接普通 32 位程序时再装
sudo apt-get install -y gcc-multilib
```

> 本项目的链接方式是 freestanding（不连 libc、直接用 `ld` 链接），
> 所以**不装 `gcc-multilib` 也能构建成功**，它只是方便你做别的 32 位实验。

### 验证安装

```bash
nasm -v
gcc -dumpversion
ld --version | head -1
make --version | head -1
qemu-system-i386 --version | head -1
```

### 本项目测试过的版本

- nasm 3.01
- gcc 15（x86_64-linux-gnu）
- GNU ld (binutils) 2.47
- GNU Make 4.4.1
- QEMU 11.1.0

---

## 三、构建与运行

```bash
make            # 生成 os.img
make run        # 用 QEMU 启动并显示内核画面
make debug      # 启动 QEMU 并停在起点等待 gdb（gdb: target remote :1234）
make clean      # 删除全部编译产物
make rebuild    # 先 clean 再重新编译
```

没有图形界面时，可用无界面方式启动：

```bash
qemu-system-i386 -drive format=raw,file=os.img -display none
```

---

## 四、文件说明

### 源文件

| 文件 | 作用 |
| --- | --- |
| `boot.asm` | 唯一引导扇区（512 字节）。用 `INT 13h` 的 LBA 扩展分两段读盘：128 扇区到 `0x80000`，2 扇区到 `0x90000`；读盘失败打印可见消息；调用 `boot_common.inc` 的 `BOOT_ENTER_PM` / `BOOT_BUILD_PAGETABLE` 完成 GDT、保护模式和 0~4MB 恒等分页；开启分页后跳到 `0x80000`。 |
| `boot_common.inc` | `boot.asm` 的公共引导实现：临时 GDT、16→32 位保护模式切换、`0x10000/0x11000` 页表与 4MB 恒等映射、BIOS 打印例程。 |
| `loader.asm` | 保护模式入口 `start`。打开 CR4.OSFXSR/OSXMMEXCPT、清 CR0.EM/TS；加载 6 项内核 GDT（ring0/ring3 代码、数据、TSS 槽），清 BSS，主栈设 `0xC000`，调用 `kernel_main`；提供 IRQ0、`int 0x40`、`int 0x80`、`#PF` 和 `#DE/#BP/#UD/#GP` 异常 stub，返回后死循环。 |
| `kernel/main/main.c` | 组合根：初始化 VGA、串口、任务、机械臂应用、IDT/PIC/PIT，最后切入第一个任务。 |
| `kernel/error/error.c` | 唯一错误屏与停机入口，打印 `owner/field/type/detail/at`。 |
| `kernel/task/task.c` | `task_struct`、任务槽、上下文切换的唯一写者。 |
| `kernel/mm/address_space/address_space.c` | 用户地址空间的唯一 owner：255 页物理池、PD/PT、VMA、brk、guard、copyin/copyout、用户缺页与 CR3 切换。 |
| `arch/x86/cpu/fault/page_fault.c` | IDT 14 号门、用户态 `#PF` 修复/终止、内核态 panic 页与 `page_fault_probe()` 的唯一写者。 |
| `arch/x86/cpu/exception/exception.c` | IDT 0/3/6/13 号门、用户 `#DE/#BP/#UD/#GP` 现场与信号投递的唯一写者；先查 `process_signal_deliver()`，无 handler 才默认终止；不安装 14 号门。 |
| `kernel/task/family/family.c` | 只维护内核任务树的父子关系、reaper 与孤儿重挂；用户进程父子关系归 `kernel/proc/process.c`。 |
| `kernel/proc/process.c` | 用户 PCB、PID、父子关系、退出码、信号 pending/SIGCHLD、`fork/wait/reap/cleanup` 的唯一写者；只持有 AS 和 task 指针；P5.2 增加 `process_slot_space()` / `process_slot_fd_info()` 按 PCB 槽只读快照；P6.0 增加 `process_last_orphan()` 最近事件快照。 |
| `kernel/proc/fd/fd.c` | 每进程 8 槽 fd 表的唯一 owner；P5.2 增加 `proc_fd_snapshot()`，逐 fd 返回 `used/kind`，不把 pipe 对象指针交给 UI。 |
| `arch/x86/cpu/interrupt/interrupt.c` | IDT、8259A PIC 重映射、PIT 100Hz、EOI 的唯一写者。 |
| `drivers/video/vga/text/vga.c` | VGA 影子显存与文本绘制。 |
| `drivers/serial/uart/uart.c` | COM1 初始化、VGA 差量帧、动态 `0x03` 状态帧、`J` / `B` / `R` / `F` 命令状态机；命令字节只在空闲时识别，变长载荷里的命令字母不会被误判。 |
| `drivers/input/keyboard/keyboard.c` | PS/2 扫描码读取。 |
| `lib/fixed/`、`lib/math/`、`lib/format/` | 定点、三角、格式化。 |
| `app/arm/kinematics/`、`control/`、`render/`、`app/` | 机械臂纯运动学、控制状态、渲染、任务装配。 |
| `app/fpu/demo/`、`app/fpu/render/` | FPU/SSE 实验的任务状态与全屏页面。 |
| `app/tree/demo/`、`app/tree/render/` | 家族树演示任务与树形页；演示任务由 `famP` 创建两个子进程后退出。 |
| `app/user/demo/` | 五个 ring3 lane、四轮 fork/wait 信号、exec 与 pipe 样本、VMA/brk/heap/guard 数据页证据。 |
| `app/user/page/` | P5.2 V 页导航唯一 owner：`page_mode`、选中 PCB 槽、10 行窗口；`Tab`/数字/方向键/`PgUp/PgDn`/`Home/End` 的解释只在这里。 |
| `app/user/render/` | P5.2 页面渲染拆分：`render.c` 公共标题/页签/状态/分派，`draw.c` 统一格式化与状态色，`slots.c`/`tree.c`/`address.c`/`fd.c` 各自只画本页；不再有单体 402 行 V 页。 |
| `drivers/console/`、`app/console/demo/`、`app/console/render/` | console 字符设备（16 字节输入队列、8×64 输出行缓存、COM1 镜像）与 X 控制台页；fork 父进程关闭自身 0/1，子进程继续读 fd0/写 fd1。 |
| `kernel.ld` | 链接脚本。加载基址 `0x80000`；BSS 当前到 `0x974BC`，由 loader 清零；`render` 栈从 `0x9000` 起，远低于内核段。 |
| `Makefile` | 构建脚本：汇编、编译、链接、打包镜像，以及运行 / 调试 / 清理。 |

### 生成的文件（`make clean` 会全部删除）

| 文件 | 说明 |
| --- | --- |
| `boot.bin` | `boot.asm` 汇编出的 512 字节引导扇区。 |
| `build/` | 各模块的 `.o` 与 `loader.o` 中间产物。 |
| `kernel.elf` | 带符号信息的 ELF，用于 `objdump` / `gdb` 调试。 |
| `kernel.bin` | 纯二进制内核镜像，被写入磁盘。 |
| `os.img` | 1.44MB 软盘镜像：第 0 扇区是 `boot.bin`，第 1 扇区起是 `kernel.bin`。 |

---

## 五、内存布局

| 物理地址 | 用途 |
| --- | --- |
| `0x00080000` – `0x00090000` | boot 第一段 LBA 读取的 128 扇区（64KB）窗口；第二段 64 扇区（32KB）读入 `0x90000`；当前 `kernel.bin` 79,016 字节，结束于 `0x934A8` |
| `0x000934C0` – `0x000974BC` | `.bss`：loader 载入后由 `rep stosb` 清零；尾部 `0x934C0..0x974BC` 低于 `AEVUM_KERNEL_END=0x98000` |
| `0x00009000` – `0x00009FFF` | `render` 任务栈（栈顶 `0xA000`，向下增长） |
| `0x0000A000` – `0x0000AFFF` | `idle` 任务栈（栈顶 `0xB000`，向下增长） |
| `0x0000B000` – `0x0000BFFF` | 内核主线程栈（栈顶 `0xC000`） |
| `0x0000C000` – `0x0000CFFF` | `probe` 任务栈（栈顶 `0xD000`，按 `P` 创建） |
| `0x0000D000` – `0x0000DFFF` | `spinA` 任务栈（栈顶 `0xE000`，按 `T` 创建） |
| `0x0000E000` – `0x0000EFFF` | `spinB` 任务栈（栈顶 `0xF000`，按 `T` 创建） |
| `0x001FF000` – `0x001FFFFF` | `control` 任务栈（栈顶 `0x200000`，向下增长） |
| `0x00200000` – `0x002BFFFF` | 用户进程内核栈池，12 个 4KB 槽；栈顶 `0x201000..0x20C000`，由 `kernel_stack_alloc()` 位图分配 |
| `0x00200000` – `0x00200FFF` | 槽 0，示例 `userA`（栈顶 `0x201000`） |
| `0x00201000` – `0x00201FFF` | 槽 1，示例 `userB`（栈顶 `0x202000`） |
| `0x00010000` / `0x00011000` | 内核页目录 / 页表（`boot.asm` 建立 0–4MB 恒等映射） |
| `0x00013000` – `0x00013FFF` | `K` 探针恢复用的 PDE 1 页表；默认 PDE 1=0，按 `R` 时临时挂上 |
| `0x00014000` – `0x00014FFF` | `K` 探针恢复用的诊断页；只承接 `0x00400000` 的写操作 |
| `0x00012000` – `0x000127FF` | 256 项 IDT（`0x20` IRQ0、`0x40` 软件调度门、`0x80` ring3 系统调用门） |
| `0x0008FC80` – `0x0008FCBF` | 内核 GDT（6 项：ring0 代码/数据、ring3 代码/数据、TSS） |
| `0x00093020` – `0x00093088` | TSS（ring3 进内核时提供 `esp0/ss0`） |
| `0x00100000` – `0x001FEFFF` | 用户物理页池，255 页位图分配；PD/PT/code/data/stack/control 与已 `brk` 的 heap 页都从这里出 |
| `0x00400000` | 用户 code 页；每个 AS 一份，物理页来自池 |
| `0x00401000` | 用户 data 页；计数器、mode、fork/wait、brk/heap/guard 结果 |
| `0x00402000` | guard 页：VMA 存在但故意无 PTE；写它触发 `#PF` |
| `0x00403000` | 用户 stack 页；栈顶 `0x00404000`，向下增长先撞 guard |
| `0x00404000` | 用户 control 页：V 停止字，只读 |
| `0x00405000` – `0x00410000` | 用户 heap：`brk` 从 `0x405000` 起增长，上限 `0x410000` |
| `0x00202000` – `0x00202FFF` | 槽 2，示例 `userC`（栈顶 `0x203000`） |
| `0x00203000` – `0x00203FFF` | 槽 3，示例 `userD`（栈顶 `0x204000`） |
| `0x00204000` – `0x00204FFF` | 槽 4，示例 `userE`（栈顶 `0x205000`） |
| `0x00205000` – `0x002BFFFF` | 槽 5..11；空闲槽由位图保持未分配，`kernel_stack_free()` 整槽清零 |
| `0x00020000` | VGA 影子显存 |
| `0x00030000` | 串口帧快照 |
| `0x000B8000` | VGA 文本显存（80×25，每字符 2 字节：字符 + 颜色） |

---

## 六、注意事项

1. **内核镜像上限 96KB 级**：`boot.asm` 分两段 LBA 读（128 + 64 扇区，共 98,304 字节）。
   当前 clean `kernel.bin` 为 79,016 字节，BSS 结束于 `0x974BC`；BSS 尾部 `0x934C0..0x974BC` 低于 `0x98000`，由 loader 按链接符号清零。新增页面文案或全局对象前必须先看 `make` 输出的字节数。
   内核已完全避开 BIOS 低内存暂存区、VGA BIOS 影子 ROM 与 `0x7C00` boot 代码；若继续增长，必须同时增加扇区数并同步 `kernel.ld`/`config/memory.h`。
2. 内核页目录 / 页表固定在 `0x10000` / `0x11000`，IDT 固定在 `0x12000..0x127FF`；内核 GDT 在 `.data` 的 `0x8FC80..0x8FCBF`，TSS 在 BSS 的 `0x93020..0x93088`；不要和内核段或任务栈重叠。
3. 按 `B`（扫描码 `0x30`）可把 `idle` 任务阻塞成 `3id B`，再按一次唤醒；这是 `BLOCKED` 状态的可控演示。
4. 按 `T`（扫描码 `0x14`）进入/退出抢占决斗；决斗使用 `spinA` / `spinB` 两个 CPU 密集型任务，只有 PIT/IRQ0 抢占才能让两边同时前进。
5. 按 `C`（扫描码 `0x2E`）在机械臂与全屏 CPU 统计页之间切换；统计页中的 CPU% 来自每个任务的 `cpu_ticks`，不是静态文本。
6. 按 `F`（扫描码 `0x21`）进入/退出全屏 `AEVUM / FPU CONTEXT LAB`；两个任务在 `XMM0` 里各自保存不同标签，屏幕的 `CTX OK` 表示切换后读回的是自己的状态。
7. 按 `G`（扫描码 `0x22`）进入/退出全屏 `AEVUM / PROCESS FAMILY TREE`；`famP` 退出后 `famA` / `famB` 会被重挂到 `idle`，右侧面板显示父 PID、子 PID 和新父亲。
8. 按 `V`（扫描码 `0x2F`）进入/退出全屏 `AEVUM / V PAGE OBSERVABILITY / P5.2`；A/B/C/D/F 都从 CPL3 的 `0x400000` 执行，但 PD/PT 和用户页都来自 `0x100000..0x1FEFFF` 的 255 页池。D 四轮 fork 都返回 8；第一轮子进程写 `CHLD` 后 `exit(7)`，父读回 `PAPA` 并显示 `WAIT OK 7 / COW OK`；第二轮子进程安装 `SIGFPE` handler 后 `1/0` 触发 `#DE`，signal frame 进入 handler、`sigreturn` 返回 `0x4002A8`，写 `HAND` 后 `in al,0x60` 触发 `#GP -> SIGSEGV(11)`，父 wait 得到 `8000000B` 并显示 `HAND OK`；第三轮子进程 `exec(1, argv, envp)`，参数为 `alpha/beta/NULL` 与 `MODE=p42/LEVEL=2/NULL`；新镜像从新栈校验四串后写 `ARGV/ENVP` 并 `exit(10)`，父 wait 得到 10 并显示 `P4 exec 8 wait 8 st 0000000A img 1 EXEC OK arg 2 env 2`；第四轮父 `pipe()` 得到 fd 2/3，fork 后父关写端、子关读端并 `exec(2, argv)`，新镜像仍持有 fd 3，写 `PIPE` 后 `exit(12)`，父 wait 得到 12 并从 fd 2 读回 4 字节，显示 `P4.3 pipe 8 wait 8 st 0000000C fd 2>3 r4 PIPE OK`。C 写 `0xB8000` 触发 `#PF`、F 写 guard `0x402000`，两者都显示 `SIG11`；A/B 继续运行。按 `V` 停止后所有用户进程被回收，没有 ZOMBIE/PID/页/fd/pipe 泄漏；`pipe obj 0`，`PHYS PAGES 20/255`。P5.2 把这一页拆成 `SLOTS / TREE / ADDRESS / FD+SIG` 四页：SLOTS 是 10 行窗口覆盖 12 个 PCB 槽，选中槽显示 KSTACK/CR3/BRK/VMA、image/argv/envc/switches/ticks、信号、children 与池统计；TREE 按 PPID 递归，根 `R0`、子节点 `+depth`，`CH` 是子进程数、`W` 是 ZOMBIE 子进程数；ADDRESS 列 6 条 VMA 与真实 PTE R/W 探针；FD/SIG 列 8 个 fd 槽的 kind/open、pipe 端点数、最近 `#DE` 与 `#PF`。为让 ZOMBIE 稳定可观察，D lane 在父 wait 前先执行 262144 次 `yield`，子进程退出后保持 ZOMBIE 数秒；实测 `SLOT 4 PID 8 PP 7 ST ZOMB EX 7`、`PID 7 CH 1 W 1`、滚动窗 `3-12/12`，wait 完成后该项仍回到 `WAIT OK`。
9. 常驻任务栈顶由 `config/memory.h` 的 `AEVUM_STACK_P1=0x200000` / `AEVUM_STACK_P2=0xA000` / `AEVUM_STACK_IDLE=0xB000` / `AEVUM_STACK_MAIN=0xC000` / `AEVUM_STACK_PROBE=0xD000` / `AEVUM_STACK_DUEL_A=0xE000` / `AEVUM_STACK_DUEL_B=0xF000` 决定；用户进程内核栈来自 `AEVUM_KSTACK_BASE=0x200000`、`AEVUM_KSTACK_COUNT=12`、`AEVUM_TASK_STACK_SIZE=0x1000` 的位图池，槽 0 栈顶 `0x201000`，槽 11 栈顶 `0x20C000`。每个槽底部 512 字节是 FXSAVE 区，实际可用栈 3.5KB。
10. 中断帧布局必须与 `loader.asm` 的 `SAVE_CTX/RESTORE_CTX` 逐槽一致；新增字段时先改汇编压栈顺序，再改 `struct irq_frame`，否则 `iret` 会恢复到错误地址。
11. `control` 栈在 `0x1FF000..0x1FFFFF`；内核镜像文件在 `0x80000..0x90AA8`，BSS 到 `0x9493C`，与 `render` 栈 `0x9000`、用户物理页池 `0x100000..0x1FEFFF` 不重叠；第一段加载窗口 128 扇区到 `0x90000`，第二段 64 扇区从 `0x90000` 起，BSS 尾部由 loader 清零。
12. 全局 `CFLAGS` 用 `-Os -mno-sse`；只有 `app/fpu/demo` 追加 `-msse2`。非 FPU 模块若让编译器生成 SSE 暂存，会和 `fxsave` 捕获任务 `XMM` 状态的时序打架，表现为 `TAG 0 / CTX LOST`。
13. `K` 的恢复只修共享内核 PD 的 PDE 1，不碰用户地址空间；恢复后 `page_fault_probe()` 撤销 PTE/PDE 并 `invlpg`。非探针内核 fault 没有 fixup，仍走 panic 停机。
14. 按 `X`（扫描码 `0x2D`）进入/退出全屏 `AEVUM / CONSOLE FD 0/1`。进入后 render 仍是唯一键盘轮询者，非 `X` 扫描码翻成 US ASCII 后进入 fd0 队列；`read(0)` 非阻塞，空队列返回 `-1`。fd1 写入 8×64 行缓存并镜像 COM1。父进程 fork 后关闭自身 0/1，子进程继续用继承端点；按 `X` 停止时父/子都以 `exit(16)` 退出，再按 `X` 清场回机械臂。
