# 变更记录

格式参照 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循语义化版本。

> 历史记录中的路径按当时结构保留，不代表当前目录。当前结构见 [STRUCTURE.md](STRUCTURE.md)。

## [Unreleased]

### Added

- 内核 P10：`process_reap()` 在释放线程组/fd/AS/PID 后，先把 `pid/ppid/exit_code/term_sig/image_id/argc/envc/comm` 写入全局 `exit_ring[4]`，再 `proc_zero()` 真正清零 PCB；回收槽直接回到 `PROC_FREE`，不再保留 DEAD 墓碑。`process_reaped_exec()`、`process_image_id()`、`process_exec_counts()` 和 `proc_reusable_child_pid()` 改从 ring 读取，V 页 `SLOT` 底部显示 `exit <pid> st <status>`。
- 内核 P11.1：`task_struct` 增加 `wake_tick`，新增 `task_block_until_timeout()`、`task_sleep_ticks()`、`task_timer_stats()`；PIT IRQ0 每 tick 调 `task_wake_expired()` 把到期 BLOCKED 任务改 READY。新增用户 ABI `eax=24 sleep(ticks)` 与 `eax=25 wait_timeout(pid,status,ticks)`，`wait_timeout` 返回子 PID/0 超时/-1，条件满足优先于超时；`process_wait()` 与 `wait_timeout` 共用 `process_wait_common()`。D lane 验证 `sleep(20)` 慢子 + `wait_timeout(5)` 超时 + `wait_timeout(50)` 回收，以及 `sleep(1)` 快子 + `wait_timeout(20)` 回收。clean `kernel.bin=78,536`，`AEVUM_KERNEL_END` 按 boot 192 扇区修正为 `0x98000`。
- 内核 P11.2：新增 `task_priority_get(tid)` / `task_priority_set(tid,priority)` 与用户 ABI `eax=26 getpriority`、`eax=27 setpriority`；`tid=0` 表示当前任务，优先级 0..3。用户态只能操作 user 任务，ZOMBIE 不接受 set；只改后续 PIT 权重，不重算历史 `vruntime`。fork 子进程继承父优先级，clone 线程继承当前线程优先级，`process_clone_thread_current()` 删除原先被忽略的 priority 参数。D lane 验证默认 1、set 3、非法 4 拒绝、内核任务拒绝、fork 子 3、clone 线程 2，写 `0x401260="PRI1"`。clean `kernel.bin=79,016`，BSS `0x934C0..0x974BC`。
- 内核 P11.3：保留 `eax=15 read` / `eax=16 write` 的非阻塞语义，新增 `eax=28 read_block(fd,user_buf,len,ticks)` 与 `eax=29 write_block(...)`；`ticks=0` 等价非阻塞，`ticks>0` 最多等待 PIT tick，成功字节数 / EOF 0 / 错误 -1 / 超时 -2。`task_struct` 增加 `waitq`，新增 `struct wait_queue`、`task_block_on()`、`wait_queue_wake_all()`、`task_io_stats()`：在 `cli` 窗口内先重查条件再查 deadline，才置 BLOCKED；广播只改 READY，不直接切任务。pipe 增加 `read_wq/write_wq`，console 输入增加 `input_wq`；`proc_fd_read_block/write_block` 做用户范围校验与 copyin/copyout。新增独立 image 3 `user/exec-io/exec_io.asm`（644B，固定 `0x400000`），D lane 用 `spawn(image_id=3)` 验证空 pipe 2 tick 超时 -2、fork 写 `IORD` 后 `read_block(...,500)` 读回 4B、填满 64B 后 `write_block(...,1,500)` 返回 1。QEMU V 页 ADDRESS `io OK`、`REAP1 6 img3 a0 e0`，C 页 `IO b 3 w 2 tmo 1`、`SLEEP b 4 w 4 tmo 6`；clean `kernel.bin=81,768`，BSS `0x93F80..0x97FDC`，余量 36B；ring3 `code_size=4,061B`，余量 35B。
- 内核 P11.4：`task_set_idle_task()` 登记唯一 idle，`task_pick_next()` 同时收集非 idle `best` 与 `idle_best`，只有全系统没有非 idle READY 时才返回 idle；`proc_idle()` 执行 `task_note_idle_hlt(); sti; hlt`，不再忙等 `task_yield()`。I 键（`0x17`）让 control/render 主动 block 40 tick 制造探针窗口，idle 到期唤醒两者、清 phase 再 yield；C 页新增 `IDLE hlt`。QEMU 基线 PIT 3,113 时 idle `TICKS 0 / CPU 0% / IDLE hlt 0`；探针后 `IDLE hlt 39`，继续到 PIT 8,043 仍为 39；P5.3 `STRESS OK`，串口无 fault/panic。新增代码使 BSS 到 `0x981FC`，越过旧 `0x98000`，因此 boot 192→208 扇区（第二段 64→80）、`AEVUM_KERNEL_END` 同步 `0x9A000`；clean `kernel.bin=82,312`，BSS 余量 3,588B，`sizeof(task_struct)=112B`。
- 内核 P11.5：`task_struct` 增加 `slice_left`，`TASK_SLICE_TICKS=4`（100Hz 下 40ms）；boot/创建/`task_pick_next()` 选中任务时重置整片。IRQ0 仍做 `task_wake_expired()`、`cpu_ticks++`、`vruntime += 64/weight`，再递减 `slice_left`；片未尽直接返回当前帧，不重选，片到 0 才进入 `task_pick_next()`。`task_yield()` 视为放弃剩余片，所以协作式 control/render 正常负载 `EXP=0`；T duel 不让出，`PREEMPTIONS/EXP` 从 0/0 增至 100/100，证明每 4 tick 重选。C 页新增 `QUANTUM 4t` 与 `EXP <n>`，`task_slice_stats()` 导出 quantum/expires/defers。I 键 idle 探针后 `EXP 109` 对 `PREEMPTIONS 100`，多出的 9 次是 idle 片到期后仍选回 idle。P5.3 `STRESS OK`、主页面 `kt 4 s5 a1 OK`、串口无 fault/panic。clean `kernel.bin=82,664`，BSS `0x94300..0x9839C` 余量 7,268B，`sizeof(task_struct)=116B`、`tasks[]=16×116B`。
- `apps/console/monitor/api/visitor.py`：访客入口、公开社区概览与隔离数据作用域；访客 Cookie 由服务端签发，前端传入的 `user_id` 不再作为身份依据。
- `/api/visitor/bootstrap` 与 `/api/community/overview`：返回访客角色、权限说明、每日额度、社区注册/在线/房间统计和角色文化；不返回任何服务器路径。
- 访客 AI 对话：`/api/chat/stream_token` 未登录也可用，按访客作用域落盘；每日 30 条，额度用尽提示登录；文件分析仍要求登录。
- 注册用户/访客各自使用 `user_data/users/<user_id>` 与 `user_data/guests/<guest_id>` 隔离目录，聊天历史从内存字典改为按作用域持久化。
- 广域网 presence 只传播 `data_scope`（`user:<id>` / `guest:<id>`），不传播绝对路径；各节点在本机解析目录。
- `/api/model` 对非本机 Host 只返回模型元信息，`/api/model/generate` 只在本机开放，避免公网拿到服务器路径和模型子进程入口。
- `src/memoria/`：Memoria C++ 内核（17 个 `.cpp`：门控、链单元、三层平衡、分层记忆、训练、生成、检查点）。
- `include/memoria/`：与源码对应的 15 个公共头文件。
- `apps/console/monitor/`：桌面监控 API、模型生成子进程与检查点发现。
- `apps/desktop/`：Electron 壳；`npm run dist:installer` 为 Inno Setup 安装包，`npm run dist:dir` 为免安装目录；窗口图标、任务栏 AppUserModelId 与安装器快捷方式图标已配置。
- `apps/desktop/installer.iss`：Inno Setup 7 脚本；中文/英文向导、程序图标、可选桌面快捷方式、开始菜单与卸载项，与 mrhua 的安装形式一致。
- `scripts/build-installer.ps1`：用已下载的 Electron dist 拼完整 app 目录后调 ISCC，绕开 electron-builder 解压 Electron 与 7z 的内存峰值。
- `apps/community/web/desktop.html`：多窗口看板；`index.html` 保留为展示页并可作看板窗口。
- `apps/community/web/styles/presence.css`：展示页与看板共用的灵体组件。
- `data/tinyshakespeare_3000.txt`：3000 行公开语料。
- `scripts/build-linux.sh`：Linux 云端编译入口（train / gen / gen_cpu）。
- `docs/memoria/`、`docs/architecture/memoria.md`、`docs/guides/development.md`：Memoria 与开发入口文档。
- `src/kernel/host/console/robot/PERFORMANCE.md`：P6.0 QEMU 速度基线，记录 100 Hz PIT、约 31.4k 次/秒调度切换和 50/38/12 CPU 账；明确标注 TCG 非实时、不能外推硬件。
- 内核用户进程 orphan/reparent 事件：`process_last_orphan()` 导出最近一次 `from/to/children[]` 快照，V 页 TREE 第 20/21 行显示。
- 内核只读关节状态帧：COM1 在 VGA 差量帧之外增加 `A5 5A 02 03 q0 q1 q2 CRC`（i32 LE，Q16.16 rad），由 `serial_bridge.py` 转成 `{type:"state", q:[...]}`；Forge 收到 q 后进入 `KERNEL q / READ ONLY`，禁用本地 PD 并显示真实 q 曲线。
- 内核手动关节模式：新增 `arm_control_set_joint_targets()`、`q_target` / `mode` / `manual_ticks`，手动分支做关节空间 PD、限位钳位和完整运动学指标；`MANUAL_TIMEOUT_TICKS=500` 用 PIT 100 Hz tick 计时，5 秒无命令回 AUTO。
- UART `J` 命令与 `0x03` 完整状态帧：`J` + 3×int16 LE（0.001 rad）下发关节目标；`A5 5A 03 13 + 19×i32 LE + CRC` 回传 q/dq/q_target/末端/误差/可操作度/sigma/null share/limit margin/mode/null_on。
- `serial_bridge.py` 双向链路：解析 `0x03` 并广播 `{type:"status"}`，新客户端补发最近状态；处理页面 `{type:"joint"}` 写回 `J`，`full_refresh` 写回 `F`；用 `status_is_sane()` 过滤 VGA 差量中的 CRC 碰撞。
- Forge 通用建模 v1：`aevum.body/1` 用 `segments` 表示任意 link/joint 树，新增 `parent`、`joint{origin,axis}`、`local{translation,rotation,scale}`、`mesh{positions,normals,color,triangles,bounds}`，并保留原 `aevum.body/0` 3R。
- Forge glTF / GLB importer：`js/model/gltf-loader.js` 支持 GLB chunk、`.gltf` + data URI / 外部 `.bin`、accessor、node matrix/TRS、多 primitive 合并、缺省法线；导入后自动生成 link/joint 树，轴优先 `extras.jointAxis`，否则按平移主轴推断。
- Forge 通用场景渲染：`js/render/scene/body-scene.js` 递归计算任意 link 世界矩阵；`buffers.js` 支持任意三角网格与关节球；`renderer.js` 增加 body 分支；`camera.js` 支持导入后自动取景。
- Forge 手动控制与内核状态面板：`MANUAL` / `MANUAL ON`、`KERNEL STATUS / 0x03`、`sendManual()`、拖动 target 自动下发；Inspector 显示 MODE、末端误差、可操作度、sigma、null share、limit margin、max |dq|、target X/Y；导入关节可编辑轴/原点。
- 示例 `interface/web/forge/samples/arm3.glb`：4,376 B 的 3R 链，3 links / 36 triangles，`kernelCompatibility()` 返回 ok。

### Changed

- 内核 `process_wait()` 的 `pid` 语义收口：`pid > 0` 必须是当前进程的直接子进程，否则立即返回 -1；`pid <= 0` 等任意直接子进程，`waitpid(0)` 不再误等 PID 0。
- 内核 G 页 reparent 面板从最多 3 个子进程扩到 4 个，`reaper` 下移；超过 4 个显示 `+N more`。
- 内核 V 页 TREE 节点上限收到第 19 行，为第 20/21 行 orphan 事件让位；第 22 行统计和第 23/24 行公共底栏不变。
- 治理窗口改为只读：`/api/governance/status|proposals|forks|requirements` 和 `/api/community/stats` 返回角色与统计；投票、提案、分叉写操作统一 403。角色文化卡片不再放在治理窗口，改到 3D 桌面「总览」的纯文字介绍里。
- 3D 桌面取消了独立的「操作手册」启动项和按钮式卡片；桌面操作与十级社区角色文化并进「总览」页，只保留文字，不再新增第二个入口。
- MrHua 首页主舞台去掉写死的 `opacity:0`，并让 `boot.js` 先直接装载 `app-shell`，避免页面只剩灵体、启动项和窗口不出现。
- FastAPI 侧接入治理只读路由；`Request` 显式注解，避免 `from __future__ import annotations` 下被当成查询参数返回 422。
- 3D 桌面的 MrHua 启动项改为 `/mrhua/?public=1`，访客先看到工作台，再按需登录；访客 AI 对话不再被 401 挡住。
- 前端聊天对 429 额度耗尽返回可读提示，不再只显示 `HTTP 429`。
- `README.md` 代码位置表改为当前真实结构；本地开发命令改到 `scripts/desktop.ps1`。
- `README.md` 内核截图改为当前 AEVUM FORGE 工作台：结构树、QEMU 内核窗口、3D 视口与 Inspector 同屏；旧的机械臂主画面不再引用，图片资源改为 `docs/assets/readme/kernel/forge-arm-workspace.png`。
- `README.md` 内核验收截图区改为“一图一段说明”，只有成对验收图并排，避免图片表格出现只有文字的空白格。
- `PROGRESS.md` 状态与新目录对齐；安装包与 Linux 产物单独标为未验收。
- `apps/desktop/README.md` 改掉 portable-only 与“必须本机 Python”的旧说法。
- 展示页与看板配色统一回纸黄系；看板保留深色终端块。
- `scripts/build.ps1`、`scripts/build-linux.sh` 的源码路径改到 `src/memoria/`。
- 内核镜像上限从 128 扇区 65,536 B 提到 130 扇区 66,560 B；`boot.asm` 改为两段 LBA 读（128 扇区到 `0x80000`，2 扇区到 `0x90000`），`Makefile` 同步上限和输出。
- `interface/web/forge/README.md` 改成“手动关节 + 完整状态 + glTF 导入”的当前说明，公开 API 表同步 `importModel` / `setManual` / kernel telemetry。

### Fixed

- `hello_torch.obj` 从仓库根目录移除。
- `src/kernel/host/console/robot` 构建中间产物 `kernel.elf`、`os.img`、`build-baseline/` 加入 `.gitignore`，本地构建目录清理出仓库。
- `.gitignore` 增加 `node_modules/`、`dist-app/`、`build/`、`*.exe`、权重与日志，避免构建产物进入开源仓库。
- 手动超时初版按 control step 计数，实测 2 秒就回 AUTO；改为 PIT `task_tick_total()` 的 500 tick 后，5 秒窗口符合设计。
- 桥接首个状态帧曾把 VGA 差量中的 `A5 5A 03 13` CRC 碰撞解析成 q2=5.9；新增 `status_is_sane()` 后丢弃非法帧。
- 示例 GLB 初版生成脚本只取 interleaved 顶点的 x 分量，导入后 bounds 全是 0.3 立方体；改为逐顶点取 xyz/nxnynz 并重新生成。

### Notes

- 模型权重、检查点和 `node_modules` 不入库；模型发布走 Release 附件或服务器目录。
- 安装包渠道从 NSIS 改为 Inno Setup 7：`dist:installer` 已产出 `Memoria-Control-Desk-Setup-0.1.0.exe`（约 95 MB），产物不进 Git。常规 `npm run dist` 仍留作内存充足构建机使用。

## [0.1.0] - 2026-11-08

### Added

- `apps/community/web/`：社区静态站首版。单文件 HTML，零依赖，零构建步骤。
- 入口文档：`README.md`、`PROGRESS.md`、`ROADMAP.md`、`start_here.md`、`CONTRIBUTING.md`、`CHANGELOG.md`。
- `CODE_OF_CONDUCT.md`：行为准则独立成文件。
- `apps/community/README.md`：平台本体的目录说明、本地预览与上线方式。

### Changed

- `README.md` 由行为准则改写为项目说明，补代码位置表与入口索引。
- `docs/index.md` 从空文件改为文档索引。

### Notes

- 站点里的社区二维码是占位，图片待放入 `apps/community/web/assets/qrcode.png`。
- `src/` 下仍无源代码；`configs/`、`deployment/`、`docs/` 其余文件为 0 字节占位。
- 服务端 API 架构未定，见 ROADMAP P2。
