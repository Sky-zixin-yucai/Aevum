# Aevum

Aevum 是一个开源 3D 角色与世界运行时。当前仓库的重点是两条线：

- **渲染引擎**：WebGL2 角色渲染、PMX/MMD、VRM/glTF、Live2D，以及地形、天气、水体、植被和 LOD。
- **内核**：可运行的 32 位 x86 裸机内核，和一套用于设计验证的 Fortuna 内核骨架。

代码使用 MPL-2.0。模型、动作和贴图保留各自授权，不因为放进仓库就自动变成 MPL-2.0。

> Aevum is an open-source runtime for 3D characters and worlds. The current repository focuses on two tracks: a WebGL2 render engine, and a kernel effort that includes a runnable 32-bit x86 bare-metal kernel plus the Fortuna kernel skeleton. Code is MPL-2.0; models, motions and textures keep their own licenses.

![晴天草地和湖](docs/assets/readme/world-clear-day.png)

当前构建的世界页面截图。地形、草地、树木、湖水和天空由引擎实时渲染。

> Screenshot from the current world build. Terrain, grass, trees, lake and sky are rendered by the engine at runtime.

## 项目结构 / Repository Layout

| 部分 | 路径 | 当前状态 |
| --- | --- | --- |
| 渲染引擎 | `engine/` | 可运行；`validate.html` 是主要验证入口 |
| 32 位裸机内核 | `src/kernel/host/console/robot/` | 可在 QEMU 运行；已完成 ring3、独立 CR3、`int 0x80`、VMA/brk/guard/copyin-copyout、COW fork、同步异常默认信号、用户 handler/signal frame/`sigreturn`、SIGCHLD/wait status、exec/fd/pipe、12 槽内核栈池、P5.3 的 100 轮 fork/wait 压测、不经过 fork 的 `spawn`、`clone/thread_exit/gettid/thread_join` 最小线程组、detach、主线程先退、主线程不 join 的整组回收、用户态与内核态 `#PF` 故障边界，以及 `V` 页 4 页观测面（槽/树/地址/fd） |
| Fortuna 内核骨架 | `src/kernel/fortuna/` | 有 boot/mm/sched/fs/net/sandbox 等模块；设计验证阶段，没有稳定 CLI |
| 串口桥 | `src/kernel/host/bridge/` | 把内核 COM1 差量帧转为 WebSocket |
| Web / 桌面呈现层 | `interface/` | world、desktop、console；不属于引擎核心 |
| 控制平面 | `control/` | 运行时登记、进程启停、状态汇总、纯内存中继 |

> The repository separates the render engine, the runnable bare-metal kernel, the Fortuna kernel skeleton, the serial bridge and the presentation layer. The engine and the robot kernel are the two parts with runnable entry points today.

## 渲染引擎 / Render Engine

### 定位

`engine/` 是独立的 WebGL2 角色渲染引擎。它不依赖桌面壳，也不把 UI 业务混进引擎目录。模型格式各自保留解析器和 GPU 管线，不把 PMX、VRM、Live2D 强行转换成同一种内部格式。

> `engine/` is a standalone WebGL2 character renderer. It does not depend on the desktop shell, and format-specific parsers and GPU pipelines stay separate.

### 当前能力

| 能力 | 实现 |
| --- | --- |
| PMX / MMD | PMX 网格、材质、骨骼、Morph 解析；VMD 采样与播放；WebGL2 蒙皮、卡通着色、球面贴图、描边 |
| VRM / glTF | GLB/VRM 解析、Humanoid 骨骼、glTF accessor、蒙皮渲染 |
| Live2D | Cubism Core、moc3、纹理、参数、drawable、motion3、表情与动作 |
| 动作 | 动作状态机、动作捕捉、动作串联、动作匹配、关节 IK、程序化动作、Mixamo 到 VRM 重定向 |
| 世界 | 地形、天气、水体、植被、LOD、世界渲染管线 |
| 图形基础设施 | WebGL2 上下文、shader、buffer、FBO、后处理、transform feedback 风场 |

图形模块在 `engine/graphics/`，是 world 和 desktop 的唯一图形 owner。草摆动使用 `ComputeSimulator` 每帧生成风场，草顶点着色器读取 `GrassWind` UBO；world 源码不再保留等价的 inline 正弦摆动。

> PMX/MMD, VRM/glTF and Live2D each keep their own parser and renderer. Shared services cover math, resources, animation, skinning and world rendering. `engine/graphics/` is the single graphics owner; grass wind is driven by a transform-feedback compute pass and consumed by the grass vertex shader.

### 目录

```text
engine/
├─ validate.html              # 渲染验证入口
├─ models.yaml                # 对外演示清单
├─ math/                      # 向量、矩阵、四元数
├─ core/                      # ECS、渲染循环、资源管线、调度、性能统计
├─ graphics/                  # WebGL2、shader、buffer/FBO、后处理、compute
├─ formats/                   # glTF/GLB、VRM、PMX、VMD、TGA、motion3 解析
├─ loaders/                   # 格式适配、重定向缓存与 LOD
├─ animation/                 # 状态机、捕捉、串联、匹配、IK、程序化动作
├─ render/                    # VRM / PMX / Live2D / VMD / Morph / 视线 / 融合
├─ character/                 # Live2D 桥接、皮肤材质、延迟渲染扩展
├─ world/                     # 地形、天气、水体、植被、LOD、世界管线
├─ legacy/orb/                # 旧 Orb 实现，按需加载
└─ vendor/                    # Live2D Cubism Core 官方运行时
```

### 运行

在仓库根目录启动静态 HTTP 服务：

```powershell
python -m http.server 8766 --directory D:\Aevum
```

渲染验证页：

```text
http://127.0.0.1:8766/engine/validate.html
```

世界页：

```text
http://127.0.0.1:8766/interface/web/world.html
```

模型路径只写在 `engine/models.yaml` 或本地 `engine/assets/models.local.yaml` 中。模型、动作和贴图资产不进 Git；换成自己的模型时只改清单，不把路径写进 JS。

> Start a static HTTP server at the repository root, then open `engine/validate.html` or `interface/web/world.html`. Model paths live in `models.yaml` or the ignored local catalog; runtime code does not hard-code asset paths.

### 截图

| 世界天气 | 世界与角色 |
| --- | --- |
| ![雨天世界](docs/assets/readme/world-rain.png) | ![白天世界与角色](docs/assets/readme/desktop/desktop-noon.png) |

### 当前边界

- 未接 PBR / MToon。
- VRM 未接 SpringBone、表情 Morph。
- PMX 脚踝使用髋部补偿，不是逐骨骼足部 IK。
- 动作融合按骨骼分区，没有接触状态权重。
- Live2D 未接 physics3、PartOpacity 和完整 Cubism Framework 控制器。
- world 的近景草、树、水和角色仍是测试级资产与材质。

> Current limits: no PBR/MToon; no SpringBone or VRM expression morphs; PMX ankle handling is a hip compensation, not per-bone foot IK; animation blending is bone-partitioned; Live2D physics3 and full Cubism Framework control are not connected; close-up world assets remain test-grade.

## 内核 / Kernel

仓库里的“内核”分两条线，不能混为一谈：

1. **可运行的裸机内核**：`src/kernel/host/console/robot/`，NASM + C，32 位 x86，当前在 QEMU 中启动。
2. **Fortuna 内核骨架**：`src/kernel/fortuna/`，按 boot、内存、调度、文件系统、网络、沙箱拆模块，用于设计验证，不是当前 Web 主链路的运行依赖。

> The repository contains two kernel tracks: a runnable 32-bit x86 bare-metal kernel under `host/console/robot`, and the Fortuna kernel skeleton under `fortuna`. Fortuna is a design-validation track, not a runtime dependency of the Web application.

### 1. 32 位裸机内核 / Bare-metal kernel

这个内核不依赖 libc，也不跑在桌面操作系统里。BIOS 引导扇区把 128 个扇区读到 `0x80000`，进入保护模式、建立 GDT、IDT、TSS 和 0–4 MB 恒等分页，然后运行内核任务。

当前进度：`P8` 加入动态内核线程。`kernel/task/kthread/kthread.c` 提供 `kthread_create/exit/self/join/stats`：内核任务在运行时申请任务槽、TID 和 `kernel_stack_alloc()` 的 4KB 栈槽，入口通过 `task_create_kernel_arg()` 携带一个内核参数；`kthread_exit` 只结束当前任务并进入 ZOMBIE，`task_reap()` 明确跳过 kthread，直到另一个内核任务 `kthread_join()` 读取退出码后才释放动态栈、PID 和任务槽。join 复用 P7.5 的 `task_block_until()`：调度器在 kthread 真正 ZOMBIE 后按等待 TID 唤醒 joiner；等待者先登记 `kthread_wait_tid`，再在 `cli` 窗口内重查，避免 lost wake。control 在启动时创建优先级 0 的 `kping`，它 yield 192 次后 `kthread_exit(5)`；control 立即 join 并进入 BLOCKED，被唤醒后写回 status 5。clean 构建 `kernel.bin=75,336B`，距 192 扇区上限 98,304B 余 22,968B。QEMU 主页显示 `kthr 4 s5 OK`；tasks dump 显示 TID 4 与 slot 3 已释放，`created=1 exited=1 reaped=1 join_blocks=1 join_wakes=1 failed=0`。

> This kernel does not use libc and does not run on top of a desktop OS. The BIOS boot sector loads 128 sectors to `0x80000`, enters protected mode, builds GDT/IDT/TSS and identity-maps 0–4 MB, then starts kernel tasks.

> Current status: P8 adds dynamic kernel threads. `kernel/task/kthread/kthread.c` exposes `kthread_create/exit/self/join/stats`: a ring0 task allocates a task slot, TID and a 4 KB stack from `kernel_stack_alloc()` at runtime, and `task_create_kernel_arg()` carries one kernel argument into the entry. `kthread_exit` only ends the current task and leaves it ZOMBIE; `task_reap()` deliberately skips kthreads until another kernel task calls `kthread_join()`, which then frees the dynamic stack, PID and task slot. Join reuses the P7.5 `task_block_until()` pattern: the scheduler wakes the joiner by waiting TID only after the kthread is truly ZOMBIE, and the waiter registers `kthread_wait_tid` before re-checking inside the `cli` window. At boot the control task creates priority-0 `kping`, which yields 192 times and exits 5; control joins immediately, blocks, and records status 5 after wake. The clean build is 75,336 bytes, 22,968 bytes below the 98,304-byte limit. QEMU shows `kthr 4 s5 OK` on the main page; the task dump shows TID 4 and slot 3 released, with `created=1 exited=1 reaped=1 join_blocks=1 join_wakes=1 failed=0`.

#### 已实现

| 模块 | 实现 |
| --- | --- |
| 启动 | `robot/boot.asm` 一次 LBA 读 128 个扇区到 `0x80000`；`loader.asm` 清 BSS、加载 6 项 GDT、启用 FPU/SSE |
| 中断 | 256 项 IDT；PIT 100Hz IRQ0；8259A PIC 重映射；`int 0x40` 软件调度门；`int 0x80` DPL3 系统调用门；`#DE/#BP/#UD/#GP` 同步异常门 |
| 任务 | 16 个固定任务槽（3 个静态内核任务 + 动态 kthread/用户任务/线程 + 余量）；12 个 4KB 动态内核栈槽由位图池分配/释放；统一中断帧；`task_yield/block/wake/exit`；优先级加权 `vruntime` 调度 |
| 动态内核线程 | `kernel/task/kthread/kthread.c` 是运行时 kthread 的唯一 owner：`kthread_create(name,entry,arg,priority)` 从栈池取动态栈并申请任务槽/TID，`kthread_exit(code)` 只结束当前任务并进入 ZOMBIE，`kthread_join(tid,status)` 用 P7.5 的 `task_block_until()` 等目标真正 ZOMBIE 后读取退出码、释放动态栈/PID/任务槽；`task_reap()` 跳过 kthread，静态 control/render/idle 仍使用固定栈，不进入动态回收路径 |
| 上下文 | 统一中断帧保存 GPR 与 `eip/cs/eflags/user_esp/ss`；`ctx.esp` 只保存帧指针；`fxsave/fxrstor` 保存 FPU/SSE，每个任务栈底预留 512 字节 FXSAVE 区；切换时更新 TSS `esp0` |
| 内存 | 内核页目录/页表固定在 `0x10000/0x11000`；用户 PD/PT 与用户页从 `0x100000..0x1FEFFF` 的 255 页位图池分配；每页有引用计数 |
| 进程模型 | `kernel/proc/process.c` 唯一维护 PCB、PID、父子关系、退出码、信号 pending/SIGCHLD、每进程 handler 表、`wait/reap`；P7.1 把 `process` 作为资源组（TGID），`task` 作为调度实体（TID），线程共享 AS/fd/信号表但各自持有内核栈、用户栈、FPU 现场；P7.2 的 detached 线程退出后由调度器延后回收，不再允许 join；P7.3 主线程先退只结束当前任务，worker 继续运行并可 join 主线程，最后由最后活线程结束整个进程；P7.4 验证主线程不 join、以 ZOMBIE 留在组内，最后线程退出时触发整组 `process_reap()`，未 join 的主线程槽也被一并释放；P7.5 把 `wait` / `thread_join` 改为关中断重查后的真正 `BLOCKED`，由调度器在任务已转 ZOMBIE 后唤醒同组/父进程组，避免 lost wake；P8 动态 kthread 使用独立于 process 的任务生命周期，不进入 PCB/线程组，由 `kthread_join()` 回收；`task.c` 只管调度/内核栈/FPU/线程组任务槽，`address_space.c` 只管 PD/PT/VMA/物理池/brk/COW |
| ring3 | 12 个用户地址空间槽；两个固定链接镜像（初始 ring3 / exec）复用同一 VA `0x400000`；V 页跑 A/B/C/D/F，X 页跑 mode 4 console，Y 页跑 mode 5 池压力（root + 8 LIVE 子进程），S 页复用同一池页外壳跑 mode 6 的 100 轮 fork/wait；D 先做四轮基础 fork/wait：第二轮用 `SIGFPE` handler 捕获 `#DE`，第三轮 fork 子进程 `exec(1, argv, envp)`，第四轮父建 pipe、fork、子 exec 后继续持有写端并写 `PIPE`，父读回并 `wait`；X 页父进程关闭自身 0/1 后，子进程用继承端点读 fd0、写 fd1；P7.1 在 spawn 后增加线程轮次：`clone` 只接受 `VM|FILES|SIGHAND|THREAD`，worker 写共享计数器后 `thread_exit`，主线程 `thread_join` 回收；第二个 worker 自 `thread_detach`，主线程 join 返回 `-1`，退出任务由调度器自动回收；P7.3 再 fork 一个子进程，子进程主线程先 `thread_exit(0)`，worker join 主线程后作为最后线程 `exit(7)`，父进程 `wait` 校验 7；P7.4 再 fork 一个子进程，主线程创建两个 worker 后不 join 直接退出，worker A 退出 5，worker B join A 后作为最后线程退出 7，父 `wait` 校验整组回收；P7.5 再 fork 一个子进程，父立即 `wait` 并在子 LIVE 时阻塞，子主线程 clone worker 后立即 join 并阻塞，worker 退出 4 后 join 返回、子写 `BLK5` 并退出 9，父被唤醒后取回 status 9 并写 `WBLK` |
| 用户 exec | `eax=13 exec(image_id, argv, envp)`：保留 PID/PPID/任务槽，新建并切换 AS，释放旧 AS；`argv/envp` 在旧 AS 仍有效时解析，最多 4 项、每串最多 31 字符，复制到新用户栈并写成 `argc, argv[], NULL, envp[], NULL, 字符串区`；新栈 `esp` 从 `0x403FC8` 起，`sigactions`/pending 清零；内嵌镜像 1 校验参数后写 `ARGV/ENVP` 并 `exit(10)`，失败码 20..25 |
| fd 与 pipe | 每进程 8 槽 fd 表：fd 0 为 console 读、fd 1 为 console 写，pipe 从 2 起；kind 区分 `CONSOLE_READ/WRITE` 与 `PIPE_READ/WRITE`，close/link 只对 pipe 端点做引用计数；fork 复制 fd 表并 acquire pipe 端点、console 端点直接继承，exec 保留 fd 表，exit、信号终止和 reap 关闭全部 fd；console 输入 16 字节 ASCII 环形队列，输出 8 行×64 字符并镜像 COM1；匿名 pipe 固定 4 个对象、每对象 64 字节非阻塞环形缓冲；`eax=14 pipe`、`15 read`、`16 write`、`17 close`，需要等待时用 `eax=28 read_block` / `29 write_block`（`ticks=0` 非阻塞，`ticks>0` 可 -2 超时 / -3 EINTR） |
| COW fork | 新 PD/PT，所有可写用户页（data/stack/heap，以及内核可写的 control 页）清 RW、置 PTE bit 9 共享；已是 COW 的页第二次 fork 时保留 COW 位；写 #PF 或 copyout 写时复制，引用计数归零才归还物理页 |
| 系统调用 | `eax=2` yield、`3` exit(code)、`4` wait(pid,status)、`5` getpid、`6` fork、`7` getppid、`8` brk、`9` peek、`10` poke、`11` sigaction(sig,handler,mask)、`12` sigreturn、`13` exec(image_id,argv,envp)、`14` pipe(user_fds)、`15` read(fd,user_buf,count)、`16` write(fd,user_buf,count)、`17` close(fd)、`18` spawn(image_id,argv,envp)、`19` clone(entry,user_stack,flags,arg)、`20` thread_exit(code)、`21` gettid()、`22` thread_join(tid,status)、`23` thread_detach(tid)、`24` sleep(ticks)、`25` wait_timeout(pid,status,ticks)、`26` getpriority(tid)、`27` setpriority(tid,priority)、`28` read_block(fd,buf,len,ticks)、`29` write_block(fd,buf,len,ticks)、`30` kill(pid,sig)、`31` sigaction_flags(sig,handler,mask,flags)；失败统一 `-1`，阻塞超时 -2、EINTR -3，内核记录调用次数和真实 `CS=0x1B` |
| 地址空间 | 6 个 VMA：code RX `0x400000`、data RW `0x401000`、guard 无权限 `0x402000`、stack RW `0x403000`、control R `0x404000`、heap RW `0x405000..0x410000`；guard 有 VMA 无 PTE |
| 同步异常 | `exception.c` 独占 IDT 0/3/6/13；CPL3 时 `#DE -> SIGFPE(8)`、`#BP -> SIGTRAP(5)`、`#UD -> SIGILL(4)`、`#GP -> SIGSEGV(11)`；有 handler 时在用户栈构造 signal frame，无 handler 才默认终止当前进程；CPL0 非 #PF 异常停机 |
| 信号与 wait | `pending` 位图 + `sigactions[32]` 存 handler/mask/flags；`fork` 继承 handler 表与 mask，不继承 pending；子进程退出给父进程置 `SIGCHLD(17)`；`eax=30 kill(pid,sig)` 只允许 self/直接子进程，`sig=0` 只查存在，拒绝 SIGSTOP；返回用户态前 `process_signal_deliver_async()` 按 IGN/DFL/handler 处理，一次一个；`eax=31 sigaction_flags` 只支持 `SA_RESTART`，旧 `eax=11` 等价 flags=0；正常 `exit(7)` status 为 `7`，默认信号终止为 `0x80000000` 按位或 `sig`，`#DE` 为 `0x80000008` |
| signal frame | 用户栈 64 字节：trampoline VA + 原 GPR + `eip/cs/eflags/esp/ss` + `sig/old_mask/magic`；`#DE` 保存 EIP 按 `F6/F7` 指令长度前移；`sigreturn` 读回并恢复原 iret 帧 |
| 故障隔离 | IDT 第 14 项 `#PF`；记录 `CR2/ERR/EIP/CS/PID`；heap 缺页零填重试、COW 写页复制重试，guard/RO/非法地址以 `SIGSEGV` 终止当前进程 |
| 内核态故障页 | `K` 键从 kernel task 写 `0x00400000`（PDE 1 未映射）触发 CPL0 `#PF`；页面显示 PID/任务名、CS/CPL、EIP、CR2、ERR、CR3、P/W/U/RSVD/I-D、fault count、recoveries 与 kernel image 范围；`R` 临时建立 PDE 1、执行完出错写并 `iret` 返回，`H` 停机 |

#### 当前验收数据

| 项 | 当前值 |
| --- | --- |
| `kernel.bin` | 75,336 字节，小于 192 扇区上限 98,304 字节（余量 22,968） |
| 加载基址 | `0x80000`；boot 读取窗口 `0x80000..0x98000` |
| BSS | `0x92660..0x9669C`，由 loader 清零；`AEVUM_KERNEL_END=0x97000` |
| 常驻任务 | `control` / `render` / `idle` |
| 用户任务 | `userA` / `userB` / `userC` / `userD` / `userF`，早期四轮 fork 的子进程复用 PID 8；D 的 joined worker TID 6、主线程 TID 7；第二个 detached worker 自 detach 后退出，join 返回 `-1`，任务槽/内核栈由调度器回收；P7.3 fork 的子进程主线程先退出、worker join 主线程后以 `exit(7)` 结束；P7.4 再 fork 一个子进程，主线程创建 A/B 两个 worker 后不 join 直接退出，A 退出 5，B join A 后作为最后线程 `exit(7)`，父 `wait` 得到 7，整组任务槽（含主 ZOMBIE）由 `process_reap()` 释放；P7.5 的父 task 在 `wait` 中进入 BLOCKED，子进程主线程与 worker 分别在 `thread_join` 和等待启动标记中推进，worker 退出 4、子主线程退出 9 后父被唤醒并回收；P8 的 `kping` 是 control 在启动时创建的动态内核线程，TID 4、动态栈槽 1、退出码 5，join 后栈槽和 TID 均归还；`Y` 页 root PID 4 + 8 个 LIVE 子进程 PID 5..12；`S` 页父 `stressA` PID 4，100 轮子进程均复用 PID 5 后回收；PD/PT 从 `0x100000` 起按池分配 |
| 用户虚拟地址 | code `0x400000`、data `0x401000`、guard `0x402000`、stack `0x403000`（栈顶 `0x404000`）、control `0x404000`、heap `0x405000..0x410000` |
| 地址空间证据 | A/B `brk 0x406000`；heap `0x405800` poke/peek `0x48454C50`；越界 `brk(0x500000)` 返回 `0xFFFFFFFF`；guard poke 返回 0 |
| COW 证据 | D `forks 4 shared 16 live 2 breaks 7`，`last AS4 old 0011C000 -> 0011B000`；子写 `CHLD`、父读回 `PAPA`，V 页显示 `COW OK`；第二次 fork 保留已有 COW 位，signal frame 写栈也触发一次拆分 |
| 故障样例 | `#PF count 3`：C 写 `0xB8000`（`cr2 000B8000 err 00000007`）、F 写 guard `0x402000`，以及 COW 子页写路径；C `FAULT/SIG11`、F `GUARD/SIG11` |
| 同步异常样例 | D 第二轮子进程 `1/0` 触发 `#DE`：`pid 8 eip 004003CE sig 8 SIGFPE hdl 1`；signal frame 进入 handler，按 2 字节 `div ecx` 推算 `sigreturn` 返回 `0x4002A8`，父 `wait 8` 得到 `00000009`，V 页显示 `HAND OK` |
| 进程闭环 | 第一轮 D `fork 8`，子 `exit(7)`，父 `wait 8` 得到 status 7；第二轮 `fork2 8`，子 handler 捕获 `#DE` 后 `exit(9)`，父 `wait 8` 得到 `9`；第三轮 `fork 8`，子 `exec(1, argv, envp)` 后 `exit(10)`，父 `wait 8` 得到 `10`；第四轮父先 `pipe()` 得到读 2/写 3，fork 后父关写端、子关读端并 `exec(2)`，镜像写 4 字节 `PIPE` 后 `exit(12)`，父 `wait 8` 得到 `12` 后从 fd 2 读回 4 字节；四次 SIGCHLD 都被 wait 消费，`pend 00000000` |
| exec 证据 | `P4 exec 8 wait 8 st 0000000A img 1 EXEC OK arg 2 env 2`；子进程 PID 8 不变、`image_id=1`；内核解析 `alpha/beta/MODE=p42/LEVEL=2`，新栈 `esp=0x403FC8`，镜像逐字节校验后写 `ARGV/ENVP` 并 `exit(10)`；旧 AS 已释放（wait 回收后 AS 0） |
| pipe 证据 | `P4.3 pipe 8 wait 8 st 0000000C fd 2>3 r4 PIPE OK`；pipe 子在 exec 后仍持有继承的 fd 3 写端，写 `PIPE` 并 `exit(12)`；父从 fd 2 读回 4 字节并校验 `0x45505049`；读写端全关后 `pipe obj 0` |
| console fd 证据 | X 页 `pid 4 child 5 out 14 child EXIT 14 CLOSE OK`；`scan 3 push 1 frames 2237`，`in 1 last a`，`out 14 uart 14`，输出行 `CON:a` / `FD01 OK`；父进程 `close0 0 close1 0 r0 -1 w1 -1`，子进程仍用继承的 fd 0/1；COM1 原始字节中同时出现 `CON:a\nFD01 OK\n` |
| 线程组证据 | D 线程轮次 `THREAD tid 6 join 0 shared 1 ok THRD det OK main OK arg OK`；`TEST ... thr OK det OK`；线程组进程 PID 7、主线程 TID 7，joined worker TID 6；第一个 worker 对共享计数器加 1 后 `thread_exit(0)`，主线程 `thread_join(6, status)` 返回 0、status 0；第二个 worker 自 `thread_detach(gettid())`，join 返回 `-1`，detach 返回 0，退出任务槽/内核栈由调度器延后回收；P7.3 子进程主线程先 `thread_exit(0)`，worker 继续运行、join 主线程并确认 status 0，最后以 `exit(7)` 结束，父进程 wait 得到 7；join/detach 的 0、-1、self、不存在 TID 全部返回 `-1`；P7.4 子进程主线程创建 A/B 两个 worker 后不 join 直接退出，A 退出 5，B join A 得到 status 5 后作为最后线程 `exit(7)`，父 wait 得到 7，tasks 表只剩父 lane 的 PID 7，子进程整组 3 个任务槽全部回收，最终仍为 `THR 1`；P7.5 的 `blk OK` 要求 `WBLK`、`wait_blocks`、`wait_wakes`、`join_blocks`、`join_wakes` 全部非零，页面同时给出 `blk 7 /1`，表示 7 次 wait 阻塞和 1 次 join 阻塞均被唤醒；P8 主页显示 `kthr 4 s5 OK`，要求 `kping` 退出码 5、动态栈使用量回到创建前、`join_blocks>0`、`reaped==1` 同时成立，tasks dump 中 slot 3 已清零、TID 4 已归还，kthread 计数为 `created=1 exited=1 reaped=1 join_blocks=1 join_wakes=1` |
| P5.3 压测证据 | 两轮独立 `S` 都是 `round 100/100 first 5 -> 5 live 1 state STRESS OK`；`PHYS 6/255`、`proc 1/12`、`AS 1/12`、`KSTK 1/12`；第一轮 ASID 1、第二轮 ASID 102，首末 PID 始终为 5 -> 5 |
| 物理池 | `PHYS PAGES 20/255`；停止后无 ZOMBIE/PID/页/fd/pipe 泄漏；signal frame、handler、exec 与 pipe 没有额外泄漏 |
| 槽池证据 | 第一轮 Y 页 `task 12/16 proc 9/12 AS 9/12 kstack 9/12 live 9 root 4 children 8/8 POOL OK frames 1826`；PID 4..12 全 LIVE，KSTACK TOP `0x201000..0x209000`；第二次进入时同批 PID/栈顶复用，ASID 10..18 只是地址空间代次，`AS 9/12` 未溢出 |
| 内核态故障样例 | `#PF task 2 render cs 00000008 cpl 0 eip 000B4BB1 cr2 00400000 err 00000002 cr3 00010000` |
| 内核态故障结果 | `P=0 W/R=1 U/S=0 RSVD=0 I/D=0`；页面进入 `RING0 FAULT - CONTROLLED PROBE`，按 `R` 后 fault 执行成功并返回主画面；kernel image 范围 `0x80000..0x97000`；多轮 V/Y/X 后累计 `fault count 8` |

`userC` 在数据页写入 `mode=1`，数到 5000 后故意写内核显存 `0xB8000`；`userF` 在 `mode=3` 直接写 guard `0x402000`。CPU 触发 `#PF`，内核读取 `CR2/ERR/EIP`，以 `process_terminate_current(SIGSEGV)` 终止并回收当前用户任务，A/B 继续运行。D 第一轮 fork 的子页首次写入走 COW `#PF` 路径，`address_space` 识别 PTE 的 COW 位后复制私有页并返回原帧重试；第二轮子进程先 `sigaction(SIGFPE, handler, 0)`，再执行 `1/0` 触发 `#DE`（faulting EIP `0x4003CE`），`exception.c` 查 handler 后在用户栈构造 signal frame，handler 计数后 `sigreturn` 按 2 字节 `div ecx` 返回到 `0x4003D0`，子进程写 `HAND` 并 `exit(9)`，父进程 `wait` 得到 `9`。第三轮子进程调用 `eax=13 exec(1, argv, envp)`，`argv` 为 `alpha/beta/NULL`、`envp` 为 `MODE=p42/LEVEL=2/NULL`。内核在旧 AS 仍有效时解析并复制字符串，按 `argc, argv[], NULL, envp[], NULL, 字符串区` 构造新栈（`esp=0x403FC8`），保留 PID 8 和任务槽，换成内嵌 exec 镜像与新的 CR3，旧 AS 释放；新镜像从新栈逐字节校验四个字符串，全部匹配后写 `ARGV/ENVP` 并 `exit(10)`，父进程 `wait` 得到 `10` 并写 `EXEC` 验收字。第四轮父进程先 `eax=14 pipe()` 拿到读 fd 2 / 写 fd 3，`fork` 时 fd 表整体复制并 acquire 两个 pipe 端点；父关闭写端，子关闭继承的读端后 `eax=13 exec(2, {"pipe","writer","FD=3"})`。exec 只换 AS/镜像，不触碰 fd 表，所以新镜像仍持有 fd 3；它解析 argv 后向 fd 3 写 `PIPE` 并 `exit(12)`。父 `wait` 得到 `12` 后从 fd 2 读回 4 字节，校验 `0x45505049`，再关闭读端，页面写 `PIPE OK`；两端都释放后 `pipe obj 0`。机械臂主画面和 V 页的 q 值保持正常，说明 COW、#PF、signal frame、`sigreturn`、exec 与 fd/pipe 路径都没有破坏调度器与 FPU 状态。

P4.4 把 fd 0/1 接到 console 字符设备：X 页进入后，键盘扫描码由 render 统一轮询并转成 US ASCII，压入 16 字节非阻塞环形队列；子进程 `read(0)` 取走按键后写 `CON:<key>`，再写 `FD01 OK` 并以 `exit(14)` 结束。父进程在 `fork` 后关闭自己的 fd 0/1，`read(0)` 和 `write(1)` 都返回 `-1`，但子进程继承的两个端点仍可读写，证明 close 只关闭调用者句柄，console 对象本身仍由多进程共享。fd 1 的输出同时进入 8×64 行缓存并原样写 COM1；X 页渲染行缓存，串口原始文本夹在 VGA 差量帧之间。最终页显示 `pid 4 child 5 out 14 child EXIT 14 CLOSE OK`、`scan 3 push 1`、`in 1 last a`、`out 14 uart 14`、`close0 0 close1 0 r0 -1 w1 -1`，按 `X` 可退出并回到机械臂页。

> P4.4 wires fd 0/1 to a console character device. In the X page, render remains the only `keyboard_poll()` consumer and translates scancode set 1 into US ASCII in a 16-byte non-blocking ring. The forked child reads fd 0, writes `CON:<key>` and `FD01 OK`, then exits 14. Its parent closes its own fd 0/1 after `fork`; both calls return `-1`, while the child's inherited handles still read and write, proving close drops only the caller's handle and the console object stays shared. fd 1 joins an 8×64 line buffer and mirrors raw bytes to COM1. The page shows `pid 4 child 5 out 14 child EXIT 14 CLOSE OK`, `close0 0 close1 0 r0 -1 w1 -1`, and `CON:a` / `FD01 OK`; `X` exits back to the arm page.

> `userC` writes the kernel VGA buffer at `0xB8000` after 5,000 iterations; `userF` writes the unmapped guard page. The kernel records `CR2/ERR/EIP`, terminates only that process with `SIGSEGV`, and leaves `userA/userB` running. D's first COW child write takes the COW `#PF` path and gets a private page. Its second child installs a `SIGFPE` handler and then divides by zero; the faulting EIP is `0x4003CE`; `exception.c` builds a user-stack signal frame, the handler runs, `sigreturn` advances past the two-byte `div ecx` to `0x4003D0`, and the child writes `HAND` and exits 9. The parent's `wait` receives 9. Its third child calls `exec(1, argv, envp)` with `alpha/beta/MODE=p42/LEVEL=2`. The kernel parses the vectors while the old AS is still active, copies the strings into a new SysV-style stack (`argc, argv[], NULL, envp[], NULL, strings`), keeps PID 8 and the task slot, swaps in the embedded exec image and a new CR3, and releases the old address space; the new image re-reads every byte from the new stack, writes `ARGV/ENVP`, and exits 10. The parent's `wait` receives 10. Its fourth child starts with `pipe()` returning read fd 2 and write fd 3. `fork` copies the whole fd table and acquires both pipe endpoints. The parent closes the write end; the child closes the inherited read end and execs image 2 with `{"pipe","writer","FD=3"}`. `exec` swaps only the AS and image, never the fd table, so the new image still owns fd 3, parses argv, writes `PIPE`, and exits 12. The parent's `wait` receives 12, reads four bytes from fd 2, verifies `0x45505049`, closes the read end, and renders `PIPE OK`; once both ends are released the page shows `pipe obj 0`. The arm page and joint values remain valid after all four paths.

P5.1 把任务/进程/地址空间槽扩到 16/12/12，并把原来 6 个固定用户内核栈改成 `0x200000..0x20BFFF` 的 12 槽 4KB 位图池：`kernel_stack_alloc()` 在创建/fork 时取槽，`kernel_stack_free()` 在失败路径和 reap 时归还，分配与释放都清零整槽。

`Y` 页让父进程连续 fork 8 个子进程并全部保持 LIVE，页面同时显示 `task 12/16`、`proc 9/12`、`AS 9/12`、`kstack 9/12`、`live 9`、`root 4`、`children 8/8`、`POOL OK` 和 12 行槽表；PID 4..12 对应栈顶 `0x201000..0x209000`。按 `Y` 停止后父 `exit(16)`、子 `exit(17)`，procs 前 9 槽 PID 归零；再次按 `Y` 仍得到 9/12/12/12，并复用 PID 4..12 与同一批栈顶，证明槽位、地址空间和 4KB 栈都完成回收与复用。

> P5.1 raises the task/process/address-space limits to 16/12/12 and replaces the six fixed user kernel stacks with a 12-slot, 4 KB bitmap pool at `0x200000..0x20BFFF`. `kernel_stack_alloc()` runs on create/fork, `kernel_stack_free()` on failure and reap, and both wipe the whole slot. The `Y` page forks eight live children, so it shows `task 12/16`, `proc 9/12`, `AS 9/12`, `kstack 9/12`, `live 9`, `root 4`, `children 8/8`, `POOL OK`, and twelve slot rows; PID 4..12 map to stack tops `0x201000..0x209000`.

> After `Y` stops the run, the parent exits 16 and children exit 17, the first nine proc slots return to PID 0; a second `Y` still reports 9/12/12/12 and reuses PID 4..12 and the same stack tops, proving slot, AS and stack recycling.

P5.2 把 `V` 页从单体渲染改成“UI 状态 + 页面绘制”分离：`include/app/user/page.h` 与 `app/user/page/page.c` 唯一持有页面模式、选中 PCB 槽和 10 行滑动窗口，`app/arm/app/app.c` 在 `user_live` 时把扫描码交给 `user_page_key()`。`Tab` 循环切页，数字 `1..4` 直达 `SLOTS/TREE/ADDRESS/FD+SIG`，`Up/Down` 移选中槽，`PgUp/PgDn` 整窗滚动，`Home/End` 跳首尾；`0xE0` 前缀由 page 模块吞掉。渲染拆成 `render.c`（公共标题/页签/状态/分派）、`draw.c`（统一十进制/十六进制与状态名颜色）、`slots.c`、`tree.c`、`address.c`、`fd.c`，每页只画自己的行布局。

观测面新增 `proc_fd_snapshot()`（逐 fd 返回 `used/kind`，不暴露 pipe 指针）与 `process_slot_space()` / `process_slot_fd_info()`（按 PCB 槽取 AS/fd，DEAD/FREE 返回空）；`process_slot_used()` 仍只数 LIVE/ZOMBIE，`POOL proc` 显式统计 `live+zomb+dead`。`TREE` 按真实 PPID 递归并统计 `CH`/`W`；`ADDRESS` 列 6 条 VMA 与真实 PTE R/W 探针；`FD/SIG` 列 8 个 fd 槽、pipe 端点数、最近 `#DE`/`#PF`。

为让 ZOMBIE 稳定可观察，D lane 在父 `wait` 前执行 262144 次 yield，实测 `SLOT 4 PID 8 PP 7 ST ZOMB EX 7`、`PID 7 CH 1 W 1`，滚动窗 `3-12/12`；wait 完成后 `TEST wait OK cow OK hand OK exec OK pipe OK`。P5.2 clean 构建 `kernel.bin=64,872`、`boot.bin=512`、`disk.img=1,474,560`；kernel `48B046EB73CB31C2378E1FBBC90C01C8812F8E62AF46883B4DABA427285664D8`，boot `D9A70CE2...`，disk `97379832...`；距 65,536 上限只剩 664 字节。

> P5.2 separates V-page navigation from drawing. `app/user/page/page.c` owns the page mode, selected PCB slot and 10-row window; `app/arm/app/app.c` forwards scancodes only while `user_live`. `Tab` cycles pages, `1..4` jump to `SLOTS/TREE/ADDRESS/FD+SIG`, `Up/Down` move the selected slot, `PgUp/PgDn` scroll ten slots, `Home/End` jump to the ends; the page module swallows the `0xE0` prefix.

> Rendering is split into `render.c` (shared title/tabs/status/dispatch), `draw.c` (shared decimal/hex/state colors), `slots.c`, `tree.c`, `address.c`, and `fd.c`. `proc_fd_snapshot()` returns only `used/kind`, never a pipe pointer; `process_slot_space()` and `process_slot_fd_info()` read AS/fd by PCB slot and return empty for DEAD/FREE slots. `process_slot_used()` still counts only LIVE/ZOMBIE, while the `POOL proc` line explicitly counts `live+zomb+dead`. `TREE` recurses over real PPIDs and counts live/ZOMBIE children; `ADDRESS` lists six VMAs with live PTE R/W probes; `FD/SIG` lists eight fd slots, pipe endpoints, and the latest `#DE`/`#PF`. The D lane yields 262144 times before waiting, so the zombie stays visible: `SLOT 4 PID 8 PP 7 ST ZOMB EX 7`, `PID 7 CH 1 W 1`, window `3-12/12`; after wait, `TEST wait OK cow OK hand OK exec OK pipe OK`. The clean build is 64,872 bytes, 664 bytes below the 65,536 limit.

P5.3 已在本地提交为 `115a31f`。这一阶段把「反复 fork/wait 100 轮」压测落在 P5.1 的池页外壳上，不新增页面：按 `S`（扫描码 `0x1F`）进入 `AEVUM / P5.3 FORK/WAIT STRESS`，`pool_demo_start(int stress)` 仍是 `poolA/stressA` 创建、模式写入和首次 yield 的唯一入口，`stress ? 6u : 5u` 选择 ring3 mode 6 或 P5.1 mode 5。父进程 `stressA` 每轮 `fork`，子进程 `exit(7)`，父进程 `wait` 并校验返回 PID 等于本轮子 PID、status 等于 7；首轮子 PID 写 `0x401100`，后续 99 轮必须复用同一 PID，全部通过才写 `0x4010C4` 的 `POOL` 魔数并保持存活，按 `S` 停止时 `exit(18)`。页面复用 `0x4010C0` 轮次和 `0x401100/0x401104` 首末 PID，不新增结构体或独立统计：实测 `round 100/100 first 5 -> 5 live 1 state STRESS OK`、`PHYS 6/255 proc 1/12 AS 1/12 KSTK 1/12`。两轮独立 `S` 都得到首末 PID `5 -> 5`、`PHYS 6/255` 与 `proc/AS/KSTK 1/12`，ASID 从 1 变为 102 后继续复用，证明 100 次子进程回收后 PID、地址空间槽、内核栈槽和物理页都回到单父进程基线。

为容纳新增代码，C 目标增加 `-ffunction-sections -fdata-sections`、链接增加 `--gc-sections`，回收约 1.3KB 不可达代码；P5.3 clean 构建 `kernel.bin=64,808`、`boot.bin=512`、`disk.img=1,474,560`，kernel `A39A6BD5301355662B49564E61C6CB58B7603D1F6C9966F66D18EEDBFA946656`，boot `D9A70CE2...`，disk `B1490110...`；`__bss_end=0x9395C`，上限余量 728 字节。

> P5.3 puts the 100-round fork/wait stress behind `S` (`0x1F`) on the P5.1 pool-page shell instead of adding a page. `pool_demo_start(int stress)` remains the only owner of `poolA/stressA` creation, mode selection and the first yield; `stress ? 6u : 5u` selects ring3 mode 6 or P5.1 mode 5. The `stressA` parent forks once per round, the child exits 7, and the parent waits and checks that the returned PID equals the child PID and the status equals 7. The first child PID goes to `0x401100` and all 99 later rounds must reuse it; only then does the parent write the `POOL` magic at `0x4010C4` and stay alive for observation. Pressing `S` again exits the parent with 18. The page reuses the P5.1 user-data slots for the round and first/last PID, with no new struct or second counter: `round 100/100 first 5 -> 5 live 1 state STRESS OK`, `PHYS 6/255 proc 1/12 AS 1/12 KSTK 1/12`. Two independent `S` runs both report first/last PID `5 -> 5`, `PHYS 6/255`, and `proc/AS/KSTK 1/12`; ASID moves from 1 to 102 and is then reused, proving PID, AS slot, kernel-stack slot and physical pages all return to the single-parent baseline after 100 reaps.

> C objects now use `-ffunction-sections -fdata-sections` and the link uses `--gc-sections`, reclaiming about 1.3 KB of unreachable code; the clean build is 64,808 bytes, 728 bytes below the limit, with `__bss_end=0x9395C`.

内核态边界单独验证：在机械臂主画面按 `K`，kernel task 主动写 `0x00400000`。该地址正好越过 0–4 MB 恒等映射，PDE 1 不存在，CPL0 触发 `#PF`（`ERR=0x2`，P=0、W=1、U=0）。panic 页先 `cli` 阻止递归 fault，再打印现场；此时按 `R`，内核临时把 PDE 1 指向 `0x13000` 页表、PTE 0 指向 `0x14000` 诊断页，执行完出错写指令后通过原异常帧 `iret` 返回，再撤销映射并重新武装 `K`。按 `H` 或遇到非探针内核 fault 仍走停机路径。

> Kernel-mode boundary: pressing `K` makes the kernel task write `0x00400000`, just past the 0–4 MB identity map. With PDE 1 absent, CPL0 raises `#PF` (`ERR=0x2`, P=0, W=1, U=0). The panic page runs `cli` first and renders the saved frame. Pressing `R` installs a temporary PDE 1/PTE 0 mapping (`0x13000` page table, `0x14000` diagnostic page), lets the faulting store execute, returns through the original exception frame with `iret`, then unmaps the page and re-arms `K`. `H`, or any non-probe kernel fault, still halts.

#### 截图

**当前机械臂工作台**

![AEVUM FORGE 机械臂工作台](docs/assets/readme/kernel/forge-arm-workspace.png)

左侧是结构树，中间是 QEMU 里的内核窗口，右侧是 3D 视口和 Inspector。内核给出 `q1 67.9°`、`q2 -76.8°`、`q3 -56.5°`；3D 视口把同一组关节角长成可见的臂，并在同一屏上显示 `TARGET/ACTUAL ANGLE`、`KP/KD`、`TORQUE`、`TRACKING ERROR 4.78 mm`。`TASK SCHED P3/31`、`nullsp ON` 和 `kthr 4 s5 OK` 说明机械臂在动的时候，任务调度、零空间投影和动态 kthread 也在跑。

> Current AEVUM FORGE workbench: structure tree, QEMU kernel window, 3D viewport and Inspector. The kernel reports `q1 67.9°`, `q2 -76.8°`, `q3 -56.5°`; the 3D viewport turns the same joint angles into a visible arm and shows target/actual angle, gains, torque and `TRACKING ERROR 4.78 mm` on the same screen. `TASK SCHED P3/31`, `nullsp ON` and `kthr 4 s5 OK` show scheduling, null-space projection and the dynamic kthread running while the arm is live.

**启动、异常与恢复**

| COW + signal frame + handler + 物理池 | 内核态 `#PF` 受控探针 |
| --- | --- |
| ![COW、signal frame 与物理池](docs/assets/readme/kernel/kernel-ring3-sigframe.png) | ![内核态 #PF 受控探针](docs/assets/readme/kernel/kernel-kernel-pf.png) |

**按 `R` 恢复后的主画面**

![恢复后的主画面](docs/assets/readme/kernel/kernel-recover-main.png)

**P4 与 P5 观测页**

| P4.2 exec argv/env 验收（PID 8 不变、arg 2/env 2） | P4.3 pipe / fd 继承验收（exec 后写端仍有效、`PIPE OK`） |
| --- | --- |
| ![P4.2 exec argv/env 验收](docs/assets/readme/kernel/kernel-p42-exec.png) | ![P4.3 pipe 验收](docs/assets/readme/kernel/kernel-p43-pipe.png) |

**P4.4 console fd 0/1 验收**

![P4.4 console 验收](docs/assets/readme/kernel/kernel-p44-console.png)

`CON:a` / `FD01 OK`；`pid 4 child 5 out 14 EXIT 14`；`close0 0 close1 0 r0 -1 w1 -1`；`out 14 uart 14`。父进程关闭自身 0/1 后，子进程仍持有继承端点。

**P5.1 槽池与动态内核栈**

![P5.1 槽池验收](docs/assets/readme/kernel/kernel-p51-pool.png)

`task 12/16 proc 9/12 AS 9/12 kstack 9/12`；`live 9`、`root 4`、`children 8/8`、`POOL OK`；PID 4..12 对应 KSTACK TOP `0x201000..0x209000`。

**P5.2 四页观测面**

| SLOTS 页（ZOMBIE PID 8，窗口 1-10/12） | TREE 页（真实 PPID，`CH 1 W 1`） |
| --- | --- |
| ![P5.2 SLOTS](docs/assets/readme/kernel/kernel-p52-slots.png) | ![P5.2 TREE](docs/assets/readme/kernel/kernel-p52-tree.png) |

| ADDRESS 页（6 条 VMA + PTE R/W） | FD/SIG 页（fd0/1、SIGFPE、#PF） |
| --- | --- |
| ![P5.2 ADDRESS](docs/assets/readme/kernel/kernel-p52-address.png) | ![P5.2 FD/SIG](docs/assets/readme/kernel/kernel-p52-fd.png) |

**P5.2 滚动窗口**

![P5.2 滚动窗口](docs/assets/readme/kernel/kernel-p52-scroll.png)

`PgDn` 后窗口 `3-12/12`，选中 ZOMBIE；四页共享选中槽，`SLOT 2..11` 全部出现，`TASK ZOMB ... AS CR3 0011A000`；空槽显示 `EMPTY` 与池统计。

**P5.3 压测**

| P5.3 第一轮 100 轮 fork/wait（PID 5 -> 5，PHYS 6/255） | P5.3 第二轮（ASID 102，PID/页/栈仍回到基线） |
| --- | --- |
| ![P5.3 第一轮压测](docs/assets/readme/kernel/kernel-p53-run1.png) | ![P5.3 第二轮压测](docs/assets/readme/kernel/kernel-p53-run2.png) |

**P7.1 最小线程组**

![P7.1 线程组验收](docs/assets/readme/kernel/kernel-p71-thread.png)

V 页 ADDRESS 显示 `THREAD tid 6 join 0 shared 1 THRD`，底部是 `TEST ... spawn OK thr OK`、`THR 1 TID 7`；worker 写共享计数器后 `thread_exit`，主线程 `thread_join` 回收；PID 7、主线程 TID 7、worker TID 6。

**P7.2 线程 detach**

![P7.2 detach 验收](docs/assets/readme/kernel/kernel-p72-thread.png)

V 页 ADDRESS 显示 `THREAD tid 6 join 0 shared 1 THRD det OK`，底部是 `TEST ... thr OK det OK`、`THR 1 TID 7`；第二个 worker 自 `thread_detach`，主线程 join 返回 `-1`；退出任务槽和内核栈由调度器延后回收。

**P7.3 主线程先退与参数矩阵**

![P7.3 主线程先退验收](docs/assets/readme/kernel/kernel-p73-thread.png)

V 页 ADDRESS 显示 `THREAD ... det OK main OK arg OK`；子进程主线程先 `thread_exit(0)`，worker join 主线程后继续运行并以 `exit(7)` 结束，父 `wait` 得到 7；非法 join/detach 参数全部返回 `-1`。

**P7.4 主线程不 join、最后线程触发整组回收**

![P7.4 线程组回收验收](docs/assets/readme/kernel/kernel-p74-thread-group.png)

V 页 ADDRESS 显示 `THREAD ... main OK arg OK grp OK`；子进程主线程创建 A/B 两个 worker 后不 join、以 ZOMBIE 留在组内；A 退出 5，B join A 后作为最后线程退出 7，父 `wait` 得到 7，`process_reap()` 释放整组。

**P7.5 阻塞 wait/join 与 lost wake 防护**

![P7.5 阻塞等待验收](docs/assets/readme/kernel/kernel-p75-block-wake.png)

V 页 ADDRESS 显示 `TEST ... det OK blk OK`、`blk 7 /1`；父在子进程仍 LIVE 时 `wait` 进入 BLOCKED；子主线程 `thread_join` 先于 worker 退出而阻塞；worker 退出 4、子写 `BLK5` 后退出 9，父被唤醒得到 status 9 并写 `WBLK`；`blk OK` 同时要求 wait/join 的阻塞与唤醒计数非零。

**P8 动态内核线程**

![P8 动态内核线程验收](docs/assets/readme/kernel/kernel-p8-kthread.png)

主页面显示 `kthr 4 s5 OK`、`TASK SCHED P3/31`；control 启动时创建 `kping`，它拿到 TID 4 和栈池槽，yield 192 次后退出 5；control 在 `kthread_join` 上阻塞，被唤醒后取回 status 5；join 释放动态栈、TID 和任务槽，页面右侧 PID 计数回到 3/31。

**P12.1 只读 ramdisk 与最小文件 ABI**

内核 rodata 内嵌 120B 只读 ramdisk，提供单层 `open(path, flags=0)`；`hello.txt` 26B、`motd.txt` 30B。image4 走 open/read/EOF/write 拒绝/close/非法 flags/不存在文件的全链路，image3 在 P11.8 成功后 spawn image4 并 wait；V/ADDRESS 显示 `REAP1 6 img 3 a0 e0`，FD 图例新增 `RAMF`。没有目录、写文件、权限或 ATA。

#### 构建与运行

需要 Linux 或 WSL2，以及 `nasm`、支持 `-m32` 的 `gcc`、`binutils`、`make`、`qemu-system-i386`：

```bash
cd src/kernel/host/console/robot
make clean && make all
make run
```

只构建镜像：

```bash
make all
# 产物：kernel.bin、disk.img、os.img
```

无界面启动：

```bash
qemu-system-i386 -drive format=raw,file=disk.img -display none
```

调试：

```bash
make debug
# 另一个终端：gdb，target remote :1234
```

> Build with NASM, 32-bit GCC, binutils and QEMU on Linux/WSL2. `make all` produces `kernel.bin`, `disk.img` and `os.img`; `make run` starts QEMU; `make debug` waits for GDB on port 1234.

#### 当前边界

- 单核、平坦段、固定 255 页物理池。
- 只有一个 120B 的内嵌只读 ramdisk 和单层 `open(path, flags=0)`；没有目录、`/`、cwd、写文件、权限或 ATA，也没有 ELF 加载。fd 表接匿名 pipe 与 ramfs，fd 0/1 已接到全局 console。已有内嵌 flat 镜像 `exec`、`argv/envp`（最多 4 项、每串最多 31 字符）、用户态 handler/signal frame/`sigreturn`、`kill(pid,sig)` 与返回用户态前的异步投递；`wait`/`thread_join` 已是阻塞等待，不是 `task_yield()` 轮询。没有信号排队、per-thread pending、进程组/会话、作业控制或用户 `sigprocmask`；`SA_RESTART` 只覆盖用户 handler + `int 0x80` 返回点这一最小场景。
- 进程上下文按当前边界已经闭环：GPR/iret、FPU/SSE、CR3、内核栈、fork/exec/信号/回收都有实现；通用 x86 的 FS/GS/TLS、AVX/XSAVE 和信号中的 FPU 现场仍未覆盖。
- `TASK_MAX=16`、`PROC_MAX=12`、`AEVUM_AS_MAX=12`、`AEVUM_KSTACK_COUNT=12`；`V` 跑 A/B/C/D/F 五个用户任务，D 做四轮 fork/wait，第三轮 fork 子进程 exec 并传 `argv/envp`，第四轮用 pipe 证明 exec 后 fd 表仍保留；`Y` 跑 root + 8 个 LIVE 子进程并验证两轮槽位/栈复用；`S` 复用同一池页跑 100 轮 fork/wait，两轮都回到 `proc/AS/KSTK 1/12`。
- P5.1 的 12 个 4KB 内核栈槽是固定池，不可按需增长，也没有 guard 页；4KB 槽含底部 512B FXSAVE 区，实际可用 3.5KB。
- P5.2 的 `V` 页固定 4 页；原第 5 页 `DEMO` 因 `kernel.bin` 超过当时 65,536B 上限被删除，旧证据压进 `ADDRESS` 页。P12.1 clean 构建为 89,064B，上限 106,496B，余量 17,432B；BSS 到 `0x99D9C`，距 `AEVUM_KERNEL_END=0x9A000` 余 612B；`--gc-sections` 已回收不可达代码，但新增页面或长文案前仍必须先算镜像体积。
- P5.3 的 `S` 与 `Y` 共用同一池页外壳并互斥。`S` 停止后再按 `Y` 可以进入，但首次进入明显变慢；目前作为观察项保留，未改代码。
- `V` 页的页面状态是单个静态实例，四页共享选中槽；`process_slot_space()` / `process_slot_fd_info()` 是只读快照，没有 seqlock，PIT 切换窗口可能看到半更新字段。ZOMBIE 观察依赖 D lane 在 wait 前 yield 262144 次，属于演示时序，不改变 wait 语义。
- fd/pipe 的旧 `read`/`write` 仍是非阻塞语义：空读且仍有写端返回 `-1`，写端全关返回 `0`，满写返回 `-1`；需要等待时用 `read_block`/`write_block`（`eax=28/29`），`ticks=0` 等价非阻塞，`ticks>0` 可返回 -2 超时或 -3 EINTR。`fork` 复制 fd 表并逐端点 acquire，`exec` 保留，`exit`/信号终止/`reap` 关闭全部 fd。
- `fork` 已改为 COW：可写用户页共享、写时复制；control 页对用户只读，但内核写停止字前也会先拆分。
- `family.c` 只维护内核任务树；用户进程父子关系由 `process.c` 维护，G 页与 V 页是两套树。
- 内核态 `#PF` 已能捕获并显示现场；只有 `K` 的受控探针可按 `R` 临时修复 PDE 1、执行完出错指令并 `iret` 返回，然后撤销映射重新武装。非探针内核 fault 仍是终止性诊断；双重 fault、栈溢出仍未处理。
- 这是教学与验证内核，不是生产内核。

> Single-core, flat segments and a fixed 255-page physical pool. No filesystem, path-based `open` or ELF loader; the fd table binds console 0/1 and anonymous pipes. An embedded flat-image `exec`, `argv/envp` (up to four entries, 31 characters each), user handlers, signal frames and `sigreturn` exist, along with `kill(pid,sig)` and asynchronous delivery before returning to ring 3; `wait` and `thread_join` block instead of polling with `task_yield()`. There are no queued/realtime signals, per-thread pending masks, process groups, sessions, job control or user `sigprocmask`, and `SA_RESTART` only covers a user handler returning through `int 0x80`. `TASK_MAX=16`, `PROC_MAX=12`, twelve address-space slots and twelve 4 KB kernel-stack slots are the hard limits; the stack pool cannot grow and has no guard page. D runs four fork/wait rounds, and `Y` forks eight live children and proves two rounds of PID/AS/stack reuse. Legacy `read`/`write` remain non-blocking: an empty read with a live writer returns `-1`, a read after all writers close returns `0`, and a full write returns `-1`; `read_block`/`write_block` (`eax=28/29`) provide the blocking path with `ticks=0` as non-blocking and `ticks>0` returning -2 on timeout or -3 on EINTR. `fork` copies and acquires endpoints, `exec` preserves the table, and exit/signal termination/reap close every fd. `fork` is copy-on-write: writable user pages are shared, an already-COW page keeps its COW bit across a second fork, and the faulting writer gets a private copy; the user-read-only control page is split before the kernel writes its stop word. `family.c` still owns only the kernel-task tree; the user-process tree lives in `process.c`. Kernel-mode page faults are captured and rendered with a full register dump. Only the controlled `K` probe can recover: `R` installs a temporary PDE 1, lets the faulting store complete, returns through `iret`, then unmaps. Unexpected kernel faults remain terminal; double faults and stack overflow still halt. Process context is closed within this boundary: GPR/iret, FPU/SSE, CR3, the kernel stack, fork/exec/signals and reap are implemented, while general x86 FS/GS/TLS, AVX/XSAVE and FPU state in signal frames are not. `S` and `Y` share one pool-page shell and are mutually exclusive; after stopping `S`, `Y` still enters but is noticeably slower, currently kept as an observation item rather than a code change. This is a teaching and validation kernel, not a production kernel.

### 2. Fortuna 内核骨架 / Fortuna kernel skeleton

`src/kernel/fortuna/` 按职责拆成 `arch/`、`boot/`、`core/`、`mm/`、`sched/`、`fs/`、`net/`、`sandbox/`、`security/`、`drivers/`、`irq/`、`ipc/` 等目录。目标是先把启动、内存、调度、文件系统、网络和沙箱的边界拆清楚，再逐块补实现和测试。

现状：

- 有模块和目录，不是完整可用的操作系统。
- 没有稳定的命令行入口；开发时通过 Python 导入模块并跑局部测试。
- 不是 Web 主链路的运行依赖。
- 设计说明见 `docs/fortuna/design.md`，使用说明见 `docs/fortuna/usage.md`。

> Fortuna is split by kernel responsibility into architecture, boot, core, memory, scheduler, filesystem, network, sandbox, security, drivers, IRQ and IPC modules. It is a design and validation skeleton, not a complete OS; there is no stable CLI yet and it is not a runtime dependency of the Web application.

### 3. 串口桥 / Serial bridge

`src/kernel/host/bridge/serial_bridge.py` 读取内核 COM1 输出的差量帧：

```text
A5 5A 01 COUNT [row col ch attr] CRC
```

然后把变化单元格转成 WebSocket 消息，供桌面「内核实况」窗口显示。没有这段桥，内核仍然会写 `0xB8000`，但桌面收不到画面。

> The bridge reads the kernel's COM1 delta frames and forwards changed text cells over WebSocket to the desktop kernel view. Without it the kernel still writes VGA memory, but the desktop cannot display the frames.

## 快速验证 / Quick Checks

渲染引擎：

```powershell
cd D:\Aevum
python -m http.server 8766 --directory D:\Aevum
```

打开：

```text
http://127.0.0.1:8766/engine/validate.html
http://127.0.0.1:8766/interface/web/world.html
```

内核：

```bash
cd src/kernel/host/console/robot
make clean && make all
make run
```

模型、动作、贴图和内核构建产物按各自目录规则管理；不要把本地资产或生成物直接提交到 Git。

> Use the engine validation page and world page for render checks. Build and run the robot kernel from its own directory. Keep local assets and generated files out of Git.

## 许可 / License

代码是 **MPL-2.0**，见 [LICENSE](LICENSE)。行为准则见 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)，参与开发见 [CONTRIBUTING.md](CONTRIBUTING.md)。仓库地址：`https://gitcode.com/AevumArk/Aevum`，维护邮箱：`open@mrhua.cn`。

角色模型、贴图和动作素材各自遵循原授权。代码许可证不覆盖这些资产。

> Code is MPL-2.0. Models, textures and motion data keep their original licenses; the code license does not relicense them.

## 相关文档 / Related Docs

- 引擎架构：`engine/ARCHITECTURE.md`
- 引擎逐文件清单：`engine/JS-MODULES.md`
- 裸机内核说明：`src/kernel/host/console/robot/README.md`
- 裸机内核施工说明：`src/kernel/host/console/robot/CONSTRUCTION.md`
- 内核构建说明：`src/kernel/host/console/robot/INTRO.md`
- Fortuna 设计：`docs/fortuna/design.md`
- 目录边界：`STRUCTURE.md`


