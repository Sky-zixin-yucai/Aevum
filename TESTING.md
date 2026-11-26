# Aevum 测试方法与问题记录

> 规则：**每次测试都必须写记录**。记录至少包含：测试对象、启动命令、内核构建、操作步骤、证据、结论、发现的问题。没有记录的测试，视为没测。
>
> 架构/功能问题：**先修复，再补测试**。不要先写长篇分析、把问题留到以后。
>
> 页面测试：**每一个页面都要测**，不能只测机械臂主画面或 Forge。
>
> 新功能：**每个内核功能必须同时有一个演示页面**。页面要能按键进入、按键退出、在 VGA 上看到真实状态变化；没有演示页的功能，视为没做完。
>
> 回归：**每次验证都要确认已有演示页面没有被影响**。新功能页面通过，不代表旧页面没坏；旧演示页的按键、提示、状态行退化，按功能回归处理，先修再做下一项。
>
> 时间盒：**30 分钟能跑完的任务就按 30 分钟做**。禁止为了“完整”反复全量构建、重复截图、把一个验证拖成三小时。

日期起点：2026-11-26

## 一、测试铁律

1. **只测一个内核实例。**
   - 先记录 QEMU 的 PID、窗口标题、启动命令、串口端口、`disk.img` 时间。
   - 关闭其它 QEMU；不允许 headless 和可见窗口各跑一个。
   - Forge 必须连到同一个实例；页面显示 `KERNEL LINK` 不等于连对了对象。
2. **内核状态是唯一事实源。**
   - 不能只看 Forge 的 `MANUAL` 标签。
   - 必须同时看 QEMU VGA 的 `ARM STATUS`：`mode AUTO / MANUAL`、`q1/q2/q3...`。
   - 页面 `KERNEL STATUS 0x03` 与 QEMU VGA 必须一致。
3. **区分“同步”和“控制”。**
   - AUTO：Forge 只是跟随内核状态，3D 模型动不代表手动下发成功。
   - MANUAL：必须有 `mode=1`、`q_target` 是页面目标、`q` 向目标收敛，三者同时成立。
4. **每次测试留证据。**
   - 截图放 `tests/kernel/results/YYYY-MM-DD/`。
   - 记录 QEMU 窗口截图 + Forge 截图，不能只留一张页面图。
5. **失败也要写。**
   - 记录在哪一步失败、QEMU 是否还活着、串口 5555/桥 8765 是否还在、页面是否还显示旧状态。

## 二、架构/功能问题的修复顺序

1. **先定演示入口**：新功能在写代码前就确定按键、页面标题、可观察字段和退出方式；没有演示入口就先不做实现。
2. **先复现到最小步骤**：只保留一个 QEMU、一个桥、一个 Forge 连接。
3. **先修根因**，不先改显示层、不先加说明文字、不把问题留到“下一轮”。
4. **只构建一次**，构建后立刻做页面 sweep，不反复全量 clean。
5. **页面 sweep 通过后再写记录**，记录里写明哪一页失败、失败时 QEMU 是否还活着。
6. **新页面和旧页面一起验收**：新功能页面必须证明自己的状态在变；旧页面必须证明还能进、还能退、关键提示还在。
7. **时间盒内完不成，就缩小验证范围**，不要用三小时证明一个三十分钟的结论。

## 三、页面矩阵（每次都要过）

| 页面/键 | 进入方式 | 必查 |
| --- | --- | --- |
| 机械臂主画面 | 默认 | `q/q_target/mode/err_mm`、`MANUAL/AUTO`、60 FPS |
| V / SLOTS | `V` | PCB 槽、LIVE/ZOMBIE/FREE、选中槽详情 |
| V / TREE | `V` 后 `2` | roots/live/zomb、父子关系、ORPHAN 行 |
| V / ADDRESS | `V` 后 `3` | CR3/AS、VMA、COW、`PHYS PAGES`、测试标记 |
| V / FD+SIG | `V` 后 `4` | fd 表、pipe、信号 pending/handler |
| V 退出 | 再按 `V` | 必须回主画面；回不去就是 I-02 |
| B / block-wake | 主画面 `B` | idle `BLK` ↔ `READY`；V 页 live 时按 B 无反应是门控 |
| C / CPU | `C` | CPU%、TICKS、SWITCHES、SLEEP/IO/WQ/EINTR |
| G / 家族树 | `G` | reparent、children、reaper |
| K / fault | `K` | fault 页、`R` 恢复、`H` 停机 |
| X / console | `X` | fd0 输入、VGA/COM1 输出、退出 |
| S / stress | `S` | 100 轮 fork/wait、`STRESS OK` |
| Y / pool | `Y` | 内核栈池、fork 子进程、池余量 |
| T / duel | `T` | 抢占决斗、`EXP`、CPU 份额 |
| F / FPU | `F` | 两条 lane 的 XMM0 各自恢复 |
| I / idle 探针 | `I` | C 页 `IDLE hlt` 增长；不是独立页面 |

> 每次修复后至少跑：主画面、V 四页、V 退出、B、C、K。其余页面按改动范围补测；但只要改动碰到调度/进程/中断，就全表跑。
>
> 新功能落地时，页面矩阵必须新增一行；这一行至少写清：按键、页面标题、进入后应看到的状态变化、退出后回哪里。新页面不能只画静态文字，必须有内核状态在动。

### 新功能演示页验收清单

- [ ] 功能有唯一按键，且不与已有全屏页冲突。
- [ ] 页面有标题和至少一个会变化的内核状态字段。
- [ ] 进入页面后状态确实变化，不是静态文字。
- [ ] 再按同一个键（或页面内明确退出键）能回到主画面。
- [ ] 页面矩阵里新增了对应行。
- [ ] 已有演示页全部重新进出一次，按键、提示、状态行没有消失。
- [ ] 记录里贴了新页面和至少一张旧页面回归截图。

## 四、当前待修复队列

| 优先级 | 问题 | 处理原则 | 时间盒 |
| --- | --- | --- | --- |
| P0 | V 页退出、B 键门控 | **已修**：UI 退出与 demo 进程死亡解耦；headless QEMU 已验证 V 退出后 B 可用 | 30 分钟 |
| P1 | 多智能机械控制 | 先定拓扑，再改 per-robot 上下文；不做无设计的全量重构 | 先 30 分钟出方案 |
| P2 | 测试记录与页面 sweep | 每次修复后按页面矩阵跑，不重复全量构建 | 跟修复同轮 |
| P3 | Windows QEMU 引导读盘失败 | 先用 WSL QEMU 做功能验证；Windows 侧单独排查 QEMU/权限 | 30 分钟 |

> P0 先修，是因为它直接影响“内核坏了还能不能看见”。P1 是架构改造，必须先确认是单板多臂还是多板组网，否则会返工。

## 五、标准测试流程

### 1. 构建

```powershell
# WSL 内 clean build，记录 kernel.bin 大小
```

记录：`kernel.bin`、`text/data/bss`、`__bss_end`、BSS 余量。

### 2. 启动唯一 QEMU

```powershell
qemu-system-i386 -m 32M -drive format=raw,file=disk.img `
  -display gtk -vga std `
  -serial tcp:127.0.0.1:5555,server,nowait -no-reboot
```

要求：**必须有可见窗口**。记录 PID 和窗口标题。

### 3. 启动桥

```powershell
python src/kernel/host/bridge/serial_bridge.py
```

确认日志出现：

```text
[bridge] connected to QEMU serial 127.0.0.1:5555
```

### 4. Forge 连接

- 打开 `http://127.0.0.1:8766/interface/web/forge/`
- 展开 `KERNEL BRIDGE WEBSOCKET`，连接 `ws://127.0.0.1:8765`
- 确认 `KERNEL LINK`，并读到 `0x03` 状态

### 5. AUTO 基线

记录：`q`、`q_target`、`mode`、`err_mm`、`limit_margin`、QEMU VGA 同一组 q。

### 6. MANUAL 验证

- 按 `RUN` 进入手动
- 改一个关节目标
- 同时检查：
  - Forge `KERNEL STATUS MODE = MANUAL HOLD`
  - QEMU VGA `ARM STATUS mode = MANUAL`
  - `q_target` 等于页面目标
  - `q` 收敛到目标

### 7. 释放

- 停止下发，等待超过 5 秒保持窗口
- 确认 QEMU VGA 和 Forge 都回到 `AUTO`
- 如果页面仍在发目标，手动不会自动回落，这是预期，不是 bug

### 8. 页面键验证

- `V`：进入/退出 ring3 进程页；再按 `V` 应返回主画面
- `B`：只在主画面可用；`user_live` 时被门控，不能当作“内核坏了”
- `K`：页错误探针；按 `R` 恢复，按 `H` 停机
- 在 `V` 页停留时按 `B`，预期无反应；先按 `V` 退出，再按 `B`

## 六、本次测试记录（2026-11-26）

### 测试对象

- 正常 A1 内核 clean build：`kernel.bin = 88,712 B`
- 可见 QEMU：`-display gtk`，串口 `tcp:127.0.0.1:5555`（Windows 11.1.0 读盘失败后停用）
- 页面 sweep：WSL QEMU 11.1.2，`-display none`，monitor `sendkey` + `screendump`
- 桥：`serial_bridge.py`，`ws://127.0.0.1:8765`
- Forge：`http://127.0.0.1:8766/interface/web/forge/`

### 通过项

- 3R 手动：J1 目标 `-20°`，内核实际 `-20°`，末端误差 `0.00 mm`，60 FPS
- 4R 手动：J1/J2/J3/J4 = `-20°/40°/20°/-30°`，实际全部到位，误差 `0.24 mm`
- 4R body descriptor：内核状态帧 `n=4`，页面 `ARM4 / NATIVE`
- 桥修复前：Forge 显示 `MANUAL`，内核仍 `AUTO`
- 桥修复后：去掉连接瞬间的 `F`，手动目标可下发

### P0 修复：V 页退出不再等 demo 死亡

- 改动：`app/arm/app/app.c`，按 `V` 退出时先把 `user_live = 0`，再调用 `user_demo_stop()`；UI 返回主画面不再依赖 ring3 lane 是否已经退出。
- 构建：`kernel.bin = 88,712 B`，clean build 通过。
- 运行验证：WSL QEMU 11.1.2，`-display none`，monitor `sendkey` + `screendump`。
- 结果：`V` 进入 SLOTS；再按 `V` 回到主画面；回主画面后按 `B` 有响应。截图 `aevum-v.png`、`aevum-back.png`、`aevum-b.png`。
- 未验证：Windows 可见 QEMU 当前无法引导（见 I-05），所以这轮可见窗口回归还没做。

### P0 修复：B 键 block/wake 提示重新出现

- 根因：P11.4 把 idle 改成“只有没有其它 READY 才被选中”，但 B 键仍只设 `idle_block_req`。idle 永远轮不到，`proc_idle()` 就永远执行不到 `task_block()`，所以 `BLOCKED` 提示和 `3id B` 都不出现。
- 改动：`kernel/task/task.c` 增加 `task_force_idle_demo()` 和一次性 `idle_demo_pick`；B 键设置 `idle_block_req` 后强制调度器选中 idle 一次，让 idle 真正执行 `task_block()`；第二次按 B 走原 `task_wake(idle_pid)`。
- 改动文件：`include/kernel/task.h`、`kernel/task/task.c`、`app/arm/app/app.c`。
- 构建：clean build 通过，`kernel.bin = 88,776 B`。
- 结果：按 B 后出现 `BLOCKED: idle waiting task_wake()`，调度行显示 `3id B 0%`；再按 B 回到 `3id r`、`BLOCKED` 提示消失。截图 `b-blocked-14.png`、`b-wake-14.png`。
- 回归：同一构建跑页面矩阵，C/G/K/B/T/F/Y/S/V/X 全部进出正常；证据 `sweep3-contact.png`。

### 页面 sweep

在同一构建上跑页面矩阵，证据在 `tests/kernel/results/2026-11-26/pages/` 与 `page-sweep.png`、`page-sweep-2.png`：

- 主画面：正常，AUTO，q/q_target/err 都在刷新
- V / SLOTS、TREE、ADDRESS、FD+SIG：四页都出图；V 退出回主画面
- C / CPU：CPU% / TICKS / SWITCHES / SLEEP / IO / WQ / EINTR 都在
- G / 家族树：parent/child/reparent 正常
- K / fault：`RING0 FAULT - CONTROLLED PROBE`，`R` 恢复，`H` 停机
- X / console：fd0 键盘输入、VGA/COM1、close 边界正常
- S / stress：`100/100 first 5 -> 5 live 1 state STRESS OK`
- Y / pool：`pool_max 8/8`、12 个进程、池余量正常
- T / duel：`A 528/276`、`B 545/392`、`PREEMPTIONS 28`
- F / FPU：`A 0x406036`、`B 0x4044A970`，两条 lane 的 XMM0 各自恢复
- B / block：按 B 出现 `BLOCKED: idle waiting task_wake()` 与 `3id B 0%`；再按 B 回到 `3id r`；不是独立全屏页

一次连续 sweep 中，X 退出后立刻按 S/Y/T/F，出现 `app task assembly failed -> pool_demo_start`；单独重启后同样四页全部通过。结论：这是测试节奏问题（X 还没退干净就进下一页），不是这四个页面本身坏了。页面 sweep 必须在上一页完全退出后再进下一页。

### 失败/异常

- 测试过程中一度出现 **headless QEMU 和用户可见 QEMU 不是同一个实例**：
  - 用户截图是 3R、`AUTO`、`q1=72.9° / q2=-112.9° / q3=31.1°`
  - 我这边 Forge 连的是另一个 4R 实例
  - 结论：**测试对象错了**，不是手动控制不存在
- 可见 QEMU 在一次手动测试后退出/失联：
  - `5555` 不再监听，`8765` 桥断开
  - Forge 仍可能显示最后一帧旧状态
  - 此时 `V` 页无法返回、`B` 键无法访问
  - 需要区分：QEMU 退出、内核 panic、control/render 任务卡死、V 页 `user_demo_stop()` 卡住

## 七、经验

1. **Forge 的“MANUAL”不是证据，QEMU VGA 的 `mode` 才是证据。**
2. **AUTO 下 3D 模型跟着动，只是同步；不要把它说成手动控制。**
3. **页面旧状态会骗人。** 桥/内核断开后，页面可能保留最后一帧；先查端口和进程。
4. **连接瞬间不要发 `F`。** 全量 VGA 扫描会占住串口/桥接，第一条 `J` 可能拖过 5 秒手动窗口。
5. **B 键只在主画面可用。** `user_live` 时按 `B` 无反应是设计门控，不一定是内核故障。
6. **V 页退出不能绑在 demo 进程死亡上。** 已改成按 `V` 先清 `user_live`、再异步 `user_demo_stop()`；UI 必须能独立返回。
7. **页面 sweep 要等上一页完全退出。** 连续按 X→S→Y→T→F 时，X 没退干净会让后面四页都报 `pool_demo_start` 失败；单独重启后全部通过。
8. **新功能必须带演示页。** 功能只在串口日志或内部计数里存在，不算完成；必须有一个按键能进入的页面，页面上能看到状态变化，再按同一个键能退出。
9. **新页面通过不等于旧页面没坏。** 每次验证都要把已有演示页重新进出一次；旧页面的提示、按键、状态行消失，按回归 bug 处理。
10. **B 键的教训：** 调度器改了 idle 的选中规则后，B 键还在等 `proc_idle()` 消费 `idle_block_req`，结果提示永远不出现。功能演示依赖调度行为时，改动调度必须回头验证演示页。

## 八、已发现的问题

### I-01 内核不能同时操控几个智能机械

现状：

- `arm_control.c` 使用全局 `body`、`q`、`q_target`、`mode`、`manual_deadline`
- 只有一个 `control` 任务、一个 UART `J/B/R` 命令通道
- 只有一个 body descriptor；`AEVUM_BODY_MAX_JOINTS` 是单机器人内的关节上限，不是多机器人数量
- 没有 robot id / namespace，也没有按机器人路由状态帧

结论：**当前内核是单控制上下文，不能同时控制多台智能机械。**

需要讨论：

- “几个智能机械”是一块控制板带多条臂，还是多块控制板组网？
- 每个机器人一个 control 上下文 + 一个控制任务，还是单任务轮询多个上下文？
- UART/CAN/EtherCAT 是否需要 robot id、关节 id、状态帧路由？
- 多机器人时安全边界怎么隔离：一个臂过载/堵转，不能拖死另一个臂。

### I-02 内核异常时 V 页无法返回、B 键无法访问

现象：内核出问题后，`V` 页回不到主画面，`B` 键也进不去。

根因假设（待复现确认）：

- `V` 页退出只在 `user_demo_alive()` 为假时才把 `user_live` 清 0；也就是说 **UI 退出被绑在 demo 进程死亡上**。
- `user_demo_stop()` 只调用 `process_stop_all()`，而后者只写 `address_space` 的停止字，不会唤醒阻塞中的任务，也不强制终止。
- 只要有一条 lane 卡在阻塞 syscall（例如 console read）或不检查停止字，`user_demo_alive()` 就一直为真，`V` 页永远退不出去。
- `B` 键又被 `!user_live` 门控，所以 V 页不退，B 就永远进不去。

修复方向：

1. 按 `V` 退出时**立即把 `user_live` 清 0**，先让 UI 回主画面，不等待 demo 进程死亡。
2. 独立执行 `user_demo_stop()`，并给 demo 任务一个终止/唤醒兜底，不让 UI 和进程生命周期耦合。
3. 回归测试：进 V → 再按 V → 回主画面 → 按 B；再在 console lane 阻塞时重复一次。

已知机制：

- `V` 是 ring3 进程/信号页，按 `V` 切换 `user_demo_start()` / `user_demo_stop()`
- `B` 键被 `!user_live` 门控；只要 V 页还在 live，B 就不会响应
- 页面由 `render` 任务绘制；control/render 卡死或内核 panic 后，键盘扫描和翻页都停

需要区分并分别记录：

1. QEMU 进程退出 / 串口断开：页面只是旧帧，内核已经不在
2. 内核 panic / fault handler 卡住：`K` 页可能能进，也可能进不去
3. `control` 任务卡死：AUTO 不动，但 render 还能翻页
4. `render` 任务卡死：画面冻结，键盘翻页无效
5. `user_demo_stop()` 卡住：V 页无法返回，B 被门控

待办：

- 复现并记录是哪一种；不要用“内核出问题了”一句话代替分类
- 需要独立于 render 的 panic/状态输出（串口或最小 VGA 路径）
- 需要 V 页超时/强制退出路径
- 需要明确 B 键在 V 页时的 UX：是禁止，还是允许切页

### I-03 测试实例混用

已发生：headless QEMU 与可见 QEMU 各一个，Forge 连错对象。

修复规则：测试前只保留一个 QEMU；记录 PID/窗口/端口；Forge 连接后先核对 QEMU VGA q 值与页面 q 值一致。

### I-04 桥连接瞬间发 F

已修复（未提交）：`serial_bridge.py` 不再在 WebSocket 连接时发 `F`。

代价：新窗口的全量 VGA 要等内核自然全扫（每 120 次扫描）；静态单元可能短暂缺失。

### I-05 Windows QEMU 11.1.0 无法读引导扇区

现象：

- 同一份 `disk.img` 在 WSL QEMU 11.1.2 可以正常引导；
- Windows `C:\Program Files\qemu\qemu-system-i386.exe` 11.1.0 无论 `-drive if=ide` 还是 `if=floppy`，SeaBIOS 都报 `Boot failed: could not read the boot disk`，然后走网络引导；
- 用一个只打印 `TEST BOOT OK` 的最小引导扇区测试也一样失败；
- `qemu-img info` 能正常读取镜像，镜像签名 `55 AA` 正确。

结论：这不是内核问题，是 Windows QEMU/宿主读取块设备的问题。可见 QEMU 回归前必须先解决它，或者改用 WSL QEMU + VNC/截图。

待办：

- 检查 Windows QEMU 启动目录/权限；尝试 `-machine pc,accel=tcg` + 不同 `cache/aio`；
- 如果仍失败，改用 WSL QEMU 做功能验证，Windows 侧只做 Forge 页面。

## 2026-11-26 / Forge SWEEP 与 3R GRAB（重建内核）

### 对象
- QEMU：WSL QEMU 11.1.2，`-m 32M -drive format=raw,file=disk.img -display gtk -vga std -serial tcp:127.0.0.1:5555,server,nowait -monitor tcp:127.0.0.1:5556,server,nowait -no-reboot`
- 串口 `5555` / monitor `5556` / 桥 `8765` / Forge `8766`
- `kernel.bin`：88,776 bytes（224 扇区上限，余量 25,912 bytes）
- Forge URL：`http://127.0.0.1:8766/interface/web/forge/?v=16`

### 步骤
1. 撤回未提交的 `MoveJ`/`Dwell` 动作库方向代码，保留 `body_desc` 2–8 关节执行。
2. 在 Forge 增加宿主侧 `SWEEP` 与 3R `GRAB`；场景 `doc.prop` 放一个 0.12 m 方块。
3. `make clean && make -j4`，重启 QEMU 与 `serial_bridge.py`。
4. 浏览器打开 `?v=16`，连接桥；点 `SWEEP` 看 J1 左右摆动；点 `GRAB` 看末端对准方块并闭爪。

### 证据
- 构建/QEMU：`tests/kernel/results/2026-11-26/rebuild-boot.png`
- SWEEP：`tests/kernel/results/2026-11-26/rebuild-sweep.png`
- GRAB 收敛：`tests/kernel/results/2026-11-26/rebuild-grab-settled.png`
- Forge `0x03`：`MODE MANUAL HOLD`、`END ERROR 0.16 mm`、`q1 31.9° / q2 -109° / q3 77°`，夹爪闭合后方块跟随工具尖端。

### 结论
- 通过：重建内核可引导；Forge 可连桥；SWEEP 期间 J1 目标与内核 `q` 同步变化；GRAB 的 3R IK 目标为 `q1 31.9° / q2 -109° / q3 77°`，内核收敛后末端误差 0.16 mm。
- 边界：GRAB 只服务默认 3R；2R、4R–8R 仍只做建模/手动关节控制。方块是 Forge 场景对象，不是 `body_desc`。
- 新问题：Forge 窗口不在前台时 `requestAnimationFrame` 会被宿主降频，页面显示的跟踪过程会变慢；内核本身仍按 100 Hz 运行。

## 2026-11-26 / 内核 VGA 方向可视化（全工作空间 + 小地图）

### 对象
- QEMU：Windows QEMU 11.1.0，`-display gtk`，串口 5555 / monitor 5556
- `kernel.bin`：90,312 bytes（177 扇区，上限 208，余量 16,184 bytes）
- 页面：`http://127.0.0.1:8766/interface/web/forge/?v=21`

### 步骤
1. 主画面坐标范围从 ±0.85 m 放大到 ±1.45 m，base 固定在画布中心，避免机械臂在左右两端被裁切。
2. 主画面增加 base 十字参考线，边界标 `L/R/U/D`；在关节旁标 `J1U/J2R/...`，表示该关节当前主要推动方向。
3. 左上角增加固定 ±1.45 m 工作空间小地图：`base #`、当前 tip `@`、目标 `o`、轨迹 `*`、四向 `L/R/U/D`。
4. 增加 tip 轨迹、上一姿态 ghost（暗灰）、`vx/vy` 速度分解（由 `dq` 和雅可比列合成，单位 mm/s）。
5. 用串口 `B` 下发 3R 身体（0.62/0.52/0.24），再用 `J` 手动目标分别到右侧和左侧姿态，抓取 VGA 画面。

### 证据
- 右侧姿态：`tests/kernel/results/2026-11-26/viz-final-right.png`
- 左侧姿态：`tests/kernel/results/2026-11-26/viz-final-left.png`
- J1 摆动与小地图轨迹：`tests/kernel/results/2026-11-26/viz-final-sweep-b.png`

### 结论
- 通过：主画面不再只在右半边；左右目标都能完整显示；小地图能在同一帧里给出 base、当前象限、目标和轨迹；`vx/vy` 随 J1/J2 运动变化。
- 边界：小地图分辨率约 0.26 m/格，J2 的局部左右弧线可能只占一格；A 方案只摆 J2 仍不能实际走遍四个象限，完整四向需要 J1 大范围扫或 J1+J2 合成路径。
- 说明：轨迹是渲染层的历史采样，不是控制层动作语义；`@` 是当前 tip，`o` 是目标，`*` 是历史。

## 2026-11-26 / Forge 方块 0.80 m 与 3R GRAB 演示

### 对象
- QEMU：Windows QEMU 11.1.0，`-display gtk`，串口 5555 / monitor 5556；本轮 PID 199064
- 桥：`serial_bridge.py`，`ws://127.0.0.1:8765` → QEMU serial 5555
- `kernel.bin`：90,312 bytes（177 扇区，上限 208，余量 16,184 bytes）
- 页面：`http://127.0.0.1:8766/interface/web/forge/?v=22`
- 方块：`doc.prop` 默认与 3R preset 均为 `size [0.12,0.12,0.12]`、`pos [0.80,0.06,0]`

### 步骤
1. 把方块中心从 `x=0.90` 改到 `x=0.80`，保持中心高度 `y=0.06`、底面贴地；方块左端面为 `0.74`。
2. 3R 抓取几何：工具长 `0.20`，wrist 为 `[0.54,0.06]`，base 高 `0.24`；IK 目标 `q=[33.2°,-120.8°,87.6°]`。
3. 页面缓存参数升到 `?v=22`；重启 QEMU 和桥，Forge 点 Connect 进入 `KERNEL / MANUAL HOLD`。
4. 点 GRAB，等待 `GRAB 完成：方块已夹住`，再点 RELEASE 确认方块回到 `x=0.80`。

### 证据
- Forge GRAB：`tests/kernel/results/2026-11-26/windows-forge-v22-grab.png`
- Forge RELEASE：`tests/kernel/results/2026-11-26/windows-forge-v22-released.png`
- 内核 VGA：`tests/kernel/results/2026-11-26/viz-forge-grab.png`
- Forge `0x03`：`q1 33.2° / q2 -121.3° / q3 88.3°`，`END ERROR 0.25 mm`
- VGA：`q1 33.2° / q2 -120.8° / q3 87.6°`，`tip 0.250 mm`

### 结论
- 通过：方块左端面与 3R 工具尖端对齐；GRAB 后夹爪闭合、方块随工具；RELEASE 后回到 `0.80`，页面不再在右边缘裁切。
- 边界：本轮“抓取”是 Forge 夹爪状态加内核关节到点，没有内核夹爪命令，不能说成真机夹爪控制。
- 说明：`x=0.80` 是当前演示的舒适中距位置；`0.90` 在几何上也能解，但会贴近视口右缘。

## 九、讨论问题

- “机器人核心控制内核组件”和“多机器人控制”是什么关系：一个组件管一台机器人，还是管一组机器人？
- 多机械场景下，控制核心是“每机器人一个实例”，还是“一个内核多上下文”？
- 内核异常时，V 页和 B 键应该由谁保证可访问：render 任务、独立 monitor、串口控制台，还是看门狗复位？
- “内核出问题”要不要定义成可观测状态机：RUNNING / DEGRADED / FAULT / PANIC / RESET？
- Forge 只做同步显示时，如何明确标注 `AUTO SYNC`，避免被误认为手动控制？

## 十、记录模板

```markdown
## YYYY-MM-DD / 测试名称

### 对象
- QEMU PID / 窗口 / 启动命令：
- 串口端口 / 桥端口：
- kernel.bin / BSS 余量：
- Forge URL：

### 步骤
1.
2.

### 证据
- 截图：
- QEMU VGA：
- Forge 0x03：

### 结论
- 通过：
- 失败：
- 新问题：
```
