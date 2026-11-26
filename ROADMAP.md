# 内核路线图

> 目标只有一个：把 `src/kernel/host/console/robot/` 的裸机内核继续往前推。
> 旧的服务端 API、平台化、长期规划不再作为路线图内容；这份文件只记录内核已经跑通什么、现在到哪、下一步是什么。
> 施工细节见 `PROCESS.md` / `CONSTRUCTION.md`，性能和体积数字见 `PERFORMANCE.md`，验收状态同步到根 `PROGRESS.md`。

## 当前目标

内核已经走完进程、地址空间、信号、exec、fd/pipe、线程、动态内核线程、阻塞调度和只读文件 ABI。P12.1 已完成；P11.9 的公平份额基线和 P11.10 / RT-0b 的 A1 before 已完成。RT-1 第一刀（`need_resched` + 最小 RT 类 + 全部唤醒路径）已完成并闭合 10ms 模型：确定性口径 4/8 过 1ms 门槛，宿主墙钟在 0.5ms 定时器分辨率下 4/8 也过 1ms；12 后台单独报告。RT-2 第一版已完成 CBS 预算账本、throttle 和 3ms 恶意记账：确定性 4/8 正常 1.8ms 控制零 miss/零预算超限；但 tick 级账本不能 2ms 硬切，PIT one-shot 宿主不稳定，默认关闭。当前下一步是 APIC/HPET one-shot 的 2ms 硬截断与 1ms 伺服；P12.2 范围未定，不提前写块设备、写路径、目录、权限或 cwd。

## 已完成

### P1–P2 进程与地址空间

- P1：建立用户进程 PCB、PID、父子关系、退出码和 `fork/exit/wait/reap` 闭环。
- P2.1：地址空间推进到 255 页物理池、6 个 VMA、`brk`、guard 和 `copyin/copyout`。
- P2.2：完成 COW fork 与物理页引用计数。

### P3–P4 信号、exec 与文件描述符

- P3.1：ring3 `#DE/#BP/#UD/#GP` 默认终止、信号 pending/SIGCHLD、wait status 编码和 V 页信号视图。
- P3.2：每进程 `sigactions`、用户栈 signal frame、`sigreturn`；修复第二次 fork 时已有 COW 页丢失 COW 位。
- P4.1：内嵌 flat 镜像 `exec`，保留 PID/任务槽，替换用户地址空间、入口、栈和信号状态。
- P4.2：`exec` 增加 `argv/envp` 解析、字符串迁移和 SysV 初始栈。
- P4.3：每进程 8 槽 fd 表、匿名 pipe、`pipe/read/write/close`；验证 exec 后 fd 表继续有效。
- P4.4：fd 0/1 接到 console，验证键盘输入、VGA/COM1 输出、fork 继承和 close 边界。

### P5–P6 槽位、压测与回收

- P5.1：16/12/12 任务/进程/AS 槽位，12 个 4KB 用户内核栈池；Y 页验证 9 个 LIVE 进程与两轮槽位/栈复用。
- P5.2：V 页 4 页分页/滚动，进程树、ZOMBIE、waiters、VMA、信号、fd 全部可观察。
- P5.3：S 页 100 轮 `fork/wait` 压测，首末 PID 均为 5，两轮 `PHYS 6/255`、`proc/AS/KSTK 1/12`。
- P6.0：`wait` 对 `pid>0` 先校验直接子进程，`pid<=0` 等任意直接子进程；记录 orphan/reparent 事件并在 V/G 页显示。

### P7 线程组

- P7.0：`spawn(image_id,argv,envp)` 不经过 fork，直接创建独立 AS/PCB/内核栈/用户栈的子进程；D lane 用 image 1 和 `wait(10)` 闭环。
- P7.1：最小线程组。`clone` 共享 VM/FILES/SIGHAND/THREAD，`thread_exit` 只结束当前任务，`thread_join` 回收同组任务，最后一个活线程退出才让进程进入 ZOMBIE。
- P7.2：`thread_detach`。detached 线程退出后由调度器延后回收任务槽和内核栈，之后 `thread_join` 返回 -1。
- P7.3：主线程先退、worker 继续并 join 主线程；非法 join/detach 参数全部返回 -1。
- P7.4：主线程不 join，最后活线程触发整组 `process_reap()`；未 join 的主 ZOMBIE 一并回收。
- P7.5：`wait` / `thread_join` 从轮询改成真正的 `BLOCKED -> WAKE`；在 `cli` 窗口内重查条件，避免 lost wake。

### P8–P10 内核线程与 PCB 回收

- P8：`kthread_create/exit/join/stats` 在运行时申请任务槽、TID 和 4KB 内核栈；`kping` 以 TID 4 退出 5，control 在 `kthread_join` 上阻塞后被唤醒并取回 status 5。
- P9：未 join 的 kthread 在 100 tick grace 后由调度器兜底回收；有人在 `kthread_wait_tid` 上等时绝不抢收。主页显示 `kt 4 s5 a1 OK`。
- P10：`process_reap()` 保存退出摘要后 `proc_zero()`，PCB 槽真正回到 `PROC_FREE`；全局 exit ring 保留最近 4 条退出记录，V 页仍能读到。

### P11 调度、阻塞与信号完整性

- P11.1：`sleep(ticks)` 与 `wait_timeout(pid,status,ticks)`；`task_struct` 增加 `wake_tick`，PIT 扫描到期任务，条件满足优先于超时。
- P11.2：`getpriority/setpriority`，优先级 0..3；fork 子进程和 clone 线程继承调用者优先级，用户态只能操作 user 任务。
- P11.3：对象等待队列与 `read_block/write_block`；pipe 读写和 console 输入各挂等待队列，超时返回 -2；旧 `read/write` 的非阻塞语义不变。
- P11.4：idle 改成真正的空闲任务，只有全系统没有非 idle READY 时才运行，执行 `sti; hlt`；C 页显示 `IDLE hlt`。
- P11.5：4 tick 显式时间片；IRQ0 每 tick 只记账并递减，片归零才重新调度；C 页显示 `QUANTUM` 与 `EXP`。
- P11.6：等待队列从 16 槽广播改成 ticket FIFO + 定向唤醒；条件重查失败再次阻塞时保留原票据；C 页显示 `WQ blk/one/all/dep`。
- P11.7：pending signal 接入阻塞点；`read_block/write_block/sleep/wait/wait_timeout/thread_join` 统一返回 -3（EINTR）；条件优先于超时，超时优先于信号。
- P11.8：`kill(pid,sig)` 与 `sigaction_flags`；`kill` 只允许 self 或直接子进程，`sig=0` 做存在性检查，拒绝 SIGSTOP；返回用户态前异步投递；最小 `SA_RESTART` 把 `eip-2` 的 `CD 80` 帧回退并重执行。image3 四段 kill 验收通过。
- P11.9：调度公平性补章。`priority 0..3` 权重改为 `8/4/2/1`，新增 64 位 `min_vruntime`，睡眠唤醒钳到下界；固定 4 tick 片，所有任务仍有正权重，不做 FIFO 式饥饿。A1 4 任务 `52.80/26.40/13.60/7.20`，2 任务 `88.80/11.20`。
- P11.10 / RT-0a：只定义实时控制模型与度量契约，不改调度器。默认控制周期 10ms、预算 2ms、截止 10ms，后台 0/4/8/12 个 CPU-bound 任务，1000 周期 × 3 次；记录唤醒延迟、抖动、执行时间、deadline miss、切换次数。RT-0b 才加观测，RT-1 才改 `need_resched`/1kHz。
- P11.10 / RT-0b：建立 ring0 控制观测台与 TSC 探针。A1 before 的 0/4/8/12 后台最坏唤醒延迟为 30.5/152.2/313.1/472.4ms，p99 为 30.4/152.0/312.9/472.3ms；1000 周期全部 deadline miss。根因是 P11.5 的 4 tick 片延迟重选：唤醒只置 READY，`slice_left > 0` 时 PIT 出口直接返回当前帧。RT-1 先做唤醒抢占，1kHz/one-shot 留给第二刀。
- P11.10 / RT-1 第一刀与闭合：`task_struct.rt` + `need_resched` + 全部唤醒路径 `task_note_wake()`；IRQ0 有抢占请求时绕过 `tick_defer`，`task_pick_next()` 先选 RT 再选 fair。PIT 保持 100Hz，未做 1kHz/CBS。确定性 `-icount` 口径 4/8 后台最大延迟 19/22us、最大抖动 185/365us、1000 周期零 miss/overrun；12 后台 24us/562us。宿主墙钟在 Windows 0.5ms 定时器分辨率下 4/8 后台最大延迟 201/405us、抖动 705/820us、零 miss；12 后台 166us/703us/零 miss、单次 overrun。15.9ms 整机暂停样本保留为 `HOST_INVALID`。正常 A1 份额不变，`kernel.bin=88,008B`、`__bss_end=0x999DC`。
- P11.11 / RT-2 第一版：`task_struct` 增加 CBS 账本字段；`task_set_rt_budget()`、`begin/end/expire/tick()`、`task_pick_next()` 跳过 `rt_throttled`，tick 补满时置 `need_resched`。`RT_BUDGET=1` 打开账本，`RT_WORK_US=1800` 为正常控制，`RT_BUDGET_ONESHOT=1` 才启用 PIT 2ms one-shot。确定性 4/8 后台正常 1.8ms 均零 miss/零预算超限；宿主 4 后台零 miss、最大延迟 285us，宿主停顿造成 `budget_overrun=6..7` 误报；宿主 8 后台三次重跑各一个有效样本、零 miss，但抖动 1.5–8.4ms。**tick 级账本不能 2ms 硬切**：3ms 恶意场景仍执行 3000us；PIT one-shot 确定性可用、宿主不稳定，默认关闭。正常 A1 份额不变，`kernel.bin=88,712B`、`__bss_end=0x99DFC`（距上限 516B）。

### P12.1 只读 ramdisk 与最小文件 ABI

- 内核内嵌 120B 只读镜像：16B 头（magic `AEVR`、version 1、count 2）+ 2 个 24B entry（16B NUL 文件名、4B offset、4B size）。
- 文件为 `hello.txt` 26B、`motd.txt` 30B；`ramfs_init()` 校验镜像并解析到静态 4 项目录，`ramfs_lookup()` 做 16B 精确匹配，`ramfs_read()` 按 offset/len 复制，EOF 返回 0，非法参数 -1。
- fd 层新增 `PROC_FD_RAMFS_FILE=5`；`open` 只接受 `flags=0`，路径逐字节 copyin，最多 16B 且必须 NUL，成功从 fd2 起分配；`read/read_block` 在 copyout 成功后才推进 offset；`write` 返回 -1；`close` 后 `read` 返回 -1。
- 新增 `eax=32 open(ebx=path, ecx=flags)`；fork 复制 file 指针和当前 offset，但父子 offset 各自独立；exec 保留 fd 表。
- image4（`user/exec-ramfs/exec_ramfs.asm`，426B，固定 `0x400000`）验证 10 条路径：打开、读取、EOF、拒绝写、关闭、关闭后读、缺失文件、非零 flags、第二个文件、成功标记。image3 在 P11.8 成功路径末尾 `spawn(image_id=4)` 并 `wait`，只有 image4 返回 0 才 `exit 0`。
- QEMU 验收：主页 `kt 4 s5 a1 OK`；V/ADDRESS 选中 userD 显示 `REAP1 6 img 3 a0 e0`、`CHILDREN live 0 zomb 0 dead 0`、`SIG pending 00000000 last 17 count 11 child 11`、`THREAD tid 6 join 0 ... ok THRD`；P5.3 仍 `round 100/100 ... STRESS OK`、`PHYS 6/255`。
- 构建：`kernel.bin=89,064B`（P11.8 为 87,080B，+1,984B）；`text 88,811B`、`data 232B`、`bss 16,796B`；`__bss_end=0x99D9C`，距 `AEVUM_KERNEL_END=0x9A000` 余 612B；镜像上限 106,496B，余 17,432B。

## 下一阶段

P12.2：范围待定。只会在明确后决定是否接块设备、写路径、目录、权限或 cwd；不在 P12.1 里提前实现。RT-1 的 10ms 模型已闭合；RT-2 第一版账本已落地，但 2ms 硬截断仍需 APIC/HPET one-shot；1ms 伺服也需要同一套高精度定时器。P12.2 的编号和范围都不被占用。

## 不在路线图里

- 大而全的功能堆叠。一个能跑通的闭环，好过五个半成品。
- 未经验证就合并的代码。没有 QEMU 退出码、页面证据或构建数字的改动，不写成“已完成”。
- 把 0 字节占位文件、只有头文件或只有计划文档算成“已完成”。
- 跨阶段提前实现。P12.2 没定范围前，不先塞块设备、写路径、目录、权限或 cwd。
- 为了赶进度牺牲已有边界。单核、进程级 pending、固定槽位预算先保持，改之前先拿证据。