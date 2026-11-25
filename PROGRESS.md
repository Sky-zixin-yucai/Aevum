# 进度

> 更新于 2026-11-25。仓库第一个提交是 2026-11-05，首版展示页进度口径是 2026-11-08；这份文件只写能核对的状态，计划写在 [ROADMAP.md](ROADMAP.md)。

## 一句话

展示页、3D 桌面、MrHua、中继、监控服务、FastAPI 身份接口和 Memoria C++ 内核已经进入同一个仓库；Windows 桌面壳已接入，`dist:installer` 已产出约 95 MB 的 Inno Setup 安装包，免安装目录同样可产出。

## 状态表

| 部分 | 位置 | 状态 | 依据 |
| --- | --- | --- | --- |
| 顶层文档 | 根目录 | 已更新 | README / PROGRESS / start_here / CONTRIBUTING / CHANGELOG |
| 展示页 | `interface/web/index.html` | 可打开 | 左侧 rail 灵体 + 连续内容；P0–P8 为原节点，P2 / P3 / P6–P8 已按当前状态补充，另加 11-17 账号与桌面节点 |
| 看板 | `interface/web/desktop.html` | 可打开 | 多窗口、启动台、展示页窗口保留 |
| 纯内存中继 | `control/relay/` | 已实现 | 会话、上下文、分片、在线计数；无落盘 |
| 监控服务 | `archive/backend-python/monitor/desktop_server.py` | 已实现 | 读检查点、训练指标、错误码；起模型子进程 |
| 串口桥接 | `src/kernel/host/bridge/serial_bridge.py` | 已实现 | VGA 差量帧 + `0x02` / `0x03` 状态帧转 WebSocket；页面 `joint` / `full_refresh` 写回内核 |
| 内核 | `src/kernel/host/console/robot/` | 维护中 | 唯一引导扇区（208 扇区两段读）、任务/进程、结构化错误、80×25 单元格、手动关节模式；P8 动态内核线程、P9 未 join kthread 的 grace 兜底、P10 PCB 清零 + exit ring、P11.1 定时阻塞 `sleep/wait_timeout`、P11.2 `setpriority/getpriority` 与 fork/clone 继承、P11.3 对象等待队列与 `read_block/write_block` 阻塞 I/O、P11.4 idle 停机与 `IDLE hlt` 探针、P11.5 4 tick 显式时间片与 `QUANTUM/EXP`、P11.6 等待队列 ticket FIFO 与 `WQ blk/one/all/dep`、P11.7 可中断阻塞 EINTR（阻塞返回 -3、`EINTR 7`、image3 五段 SIGCHLD 验收）均已验收 |
| Forge | `interface/web/forge/` | 可打开 | 原生 3R 建模/控制；glTF/GLB 导入、任意 link/joint 树、手动关节 + `0x03` 状态 |
| Memoria C++ | `src/architecture/memoria/` + `src/architecture/memoria/include/` | 可编译 | 17 个 `.cpp`、17 个头文件；训练/生成/检查点 |
| 桌面壳 | `interface/desktop/` | 可打包 | Electron 窗口、拉起监控；`dist:installer` 出 Inno Setup，`dist:dir` 出目录 |
| 训练语料 | `data/tinyshakespeare_3000.txt` | 已入库 | 3000 行公开文本，不包含权重 |
| 模型权重 | 不入库 | 明确边界 | `.gitignore` 挡 `*.pt`、`*.safetensors`、`checkpoint*/`、`build/` |
| fortuna | `src/kernel/fortuna/` | 有代码 | 内核与引导相关实现、文档；仍与 `src/kernel/host/console/robot` 并行维护 |
| libertas | `src/architecture/libertas/` | 有代码 | 身份、安全、工具、回路等模块；不再是空骨架 |
| 3D 桌面 | `interface/web/desktop.html` | 可打开 | 窗口、启动项先渲染；VRM / PMX 角色随后载入；桌面操作和十级角色文化并进「总览」纯文字 |
| MrHua 与订阅 | `interface/web/mrhua/`、`interface/console/shell/` | 已接入 | MrHua 是 3D 桌面启动项；订阅走 3D 原生窗口，不用 iframe |
| 社区治理只读 | `archive/backend-python/monitor/api/governance.py`、`mrhua/partials/windows/ai/governance.html` | 已接入 | `/api/governance/status|proposals|forks|requirements`、`/api/community/stats` 只读；投票/提案/分叉写操作 403，角色文化不在治理页 |
| 账号 | `archive/backend-python/monitor/auth_api.py`、`src/architecture/libertas/identity/` | 已接入 | 主账号 / 子用户账号、会话、身份档案隔离；本机验证码登录 |
| 访客入口 | `archive/backend-python/monitor/api/visitor.py` | 已接入 | `/api/visitor/bootstrap`、`/api/community/overview`；访客 Cookie、每日额度、隔离数据作用域 |
| 访客 AI 与数据目录 | `archive/backend-python/monitor/chat_api.py` | 已接入 | 未登录可对话；按 `data_scope` 落盘到 `user_data/users` 或 `user_data/guests`；WAN 只传作用域 |
| 部署编排 | `ops/deploy/` | 可用 | systemd 与平台部署脚本分开存放 |
| 配置样例 | `configs/` | 空骨架 | 占位文件 |
| CI | `.github/workflows/` | 空骨架 | 占位文件 |

## 数字口径

统计时排除 `.git`、`node_modules`、`build`、`dist*`：

```powershell
$all = Get-ChildItem -Recurse -File -Force | Where-Object {
  $_.FullName -notlike '*\.git\*' -and
  $_.FullName -notlike '*node_modules*' -and
  $_.FullName -notlike '*\build\*' -and
  $_.FullName -notlike '*\dist*'
}
"$($all.Count) 个文件 / $(($all | Where-Object { $_.Length -gt 0 }).Count) 个非空"
```

## 已完成

- 展示页改成连续工作台结构：左侧 rail 常驻灵体、状态与锚点，右侧用 `.chain` 和行式清单代替卡片展台。
- 看板与展示页共用 `styles/presence.css`，灵体在 rail 和 dock 各有位置。
- 配色回到纸黄系（展示页与看板 token 已替换），保留深色终端块作为局部面板。
- 中继、串口桥接、内核入口分别收在 `control/relay/`、`src/kernel/host/bridge/`、`src/kernel/host/console/robot/`。
- 内核 COM1 先加只读 q 帧，再扩到手动关节模式与 `0x03` 完整状态帧；QEMU 串口用 TCP 5555，Forge 可下发 `J` 关节目标并显示误差/可操作度/sigma/null share/limit margin，5 秒无命令回 AUTO。
- Forge 从固定 3R 观测页扩成通用建模台：`aevum.body/1`、glTF/GLB importer、任意三角网格渲染、自动 link/joint 树、Inspector 关节轴/原点编辑、导入自动取景；只有 3R 兼容链接内核。
- Memoria C++ 内核迁入 `src/architecture/memoria/`，公共头文件在 `src/architecture/memoria/include/`；统一入口为 `src/architecture/memoria/include/aevum_core.h`，源码用 `#include <include/aevum_core.h>`，构建提供 `-I src/architecture/memoria`。
- Electron 桌面壳接入 `interface/desktop/`；`npm run dist:installer` 出 Inno Setup 安装包，`npm run dist:dir` 为免安装目录；安装包约 95 MB，产物不进 Git。
- 3000 行公开语料入库；模型权重、构建产物、`node_modules` 明确忽略。
- 3D 桌面接入 VRM / PMX，窗口与程序先渲染、角色后载入；MrHua 作为启动项，订阅走原生窗口。
- FastAPI + Uvicorn 身份接口、主账号 / 子用户隔离、本机验证码登录接入；历史内容目录尚未按用户彻底分区。
- 访客入口接入：服务端签发 `mrhua_visitor` Cookie，访客可用 AI 对话（每日 30 条），聊天历史按 `data_scope` 落到独立目录；WAN presence 只传播不透明作用域，不传播服务器路径。
- `/api/model` 对非本机请求只返回模型元信息；`/api/model/generate` 只在本机开放，避免公网拿到路径和模型入口。
- 内核 P6.0 完成细节收口：`process_wait()` 的 `pid>0` 先校验直接子进程，非子进程/不存在立即 -1；`pid<=0` 等任意直接子进程。父进程退出时记录最近一次 orphan/reparent 事件，V 页 TREE 第 20/21 行显示，G 页 reparent 面板支持 4 个子进程和 `+N more`。clean `kernel.bin=66,248` 字节，上限 66,560，余量 312 字节；速度基线见 `src/kernel/host/console/robot/PERFORMANCE.md`。
- 内核 P7.1–P7.5 已验收：最小线程组 `clone/thread_exit/gettid/thread_join`、`thread_detach` 自动回收、主线程先退后 worker 继续、主线程不 join 时最后线程触发整组 `process_reap()`，以及阻塞 `wait` / `thread_join` 的 lost wake 防护；V 页同时给出线程组、detach、阻塞计数和整组回收证据。
- 内核 P8 已验收：`kthread_create/exit/join/stats` 在运行时申请任务槽、TID 和 4KB 内核栈槽；`kping` 以 TID 4、动态栈槽退出 5，control 在 `kthread_join` 上阻塞后被唤醒并取回 status 5，join 释放动态栈、TID 和任务槽。QEMU 主页显示 `kthr 4 s5 OK`，tasks dump 中 slot 3 清零；clean `kernel.bin=75,336B`，上限 98,304B，余量 22,968B。
- 内核 P9 已验收：未 join 的 kthread ZOMBIE 在 100 tick（100Hz 下 1 秒）grace 后由调度器兜底回收，释放 4KB 动态栈、TID 和任务槽；有人在 `kthread_wait_tid` 上等时绝不抢收。`kthread_stats` 增加 `auto_reaped`，`live=created-reaped` 守恒。QEMU 主页显示 `kt 4 s5 a1 OK`，同时证明正常 join 拿到 status 5、`korph` 被自动回收 1 次；clean `kernel.bin=77,160B`，上限 98,304B，余量 21,144B。
- README 内核截图改为当前 AEVUM FORGE 工作台：左侧结构树、中间 QEMU 内核窗口、右侧 3D 视口与 Inspector 同屏；旧的机械臂主画面不再引用。

## 缺口（按优先级）

1. **安装包只在本机验收**。`dist:installer` 用 Inno Setup 的 ISCC 编译成功，形式与 mrhua 一致；安装器尚未做静默安装/卸载实跑，建议在干净 Windows 用户目录再验一次。
2. **Linux 安装包未做**。Linux C++ 编译脚本已补，但未在真实 Linux 主机跑过；PyInstaller 监控的 Linux 产物也未生成。
3. **云端模型已接对话，视频工具默认关闭**。`model_runtime_mode: cloud` 时对话经监控服务代理，按 `data_scope` 隔离；`web_search` 只给登录用户；`generate_media` 需要 `video_generation.enabled` 与真实端点，默认不开放。
4. **训练参数未调低**。当前仍是 `dim=64 / seq=16 / batch=32 / vocab=3000`，约 1,352,744 参数；下一轮按目标值调整。
5. **`docs/` 多数文件仍为空**。本轮只补 Memoria 与开发入口，其余按需填写。
6. **账号与登录只完成基础闭环**。主账号、子用户账号、会话和身份档案已隔离；邀请/邮件准入、帖子检索、回复层级、签名与自动更新仍未完成。聊天已按 `data_scope` 分区；上传、记忆等历史目录尚未按作用域彻底分区。
7. **AI 动作桥还没有接运行时**。`tools/ai-bridge` 只完成“模型文本 -> 33 维标签向量 -> 带时区 JSON”，参数化树、YAML 注册表、JSON 记录提取、成功判定和动画驱动都未落地；现状与约束见 [docs/architecture/ai-action.md](docs/architecture/ai-action.md)。
8. **代码结构还没有整理好**。公共头文件刚改为 `src/architecture/memoria/include/` 直属、源码统一 `<include/aevum_core.h>`，后续仍要整理 AI 动作桥的正式落点、模块边界和导入路径。

## 更新规则

- 模块状态变化时：改状态表对应行，改顶部日期，必要时重算数字口径。
- 新增功能先更新 CHANGELOG，再改 PROGRESS。
- 不写没有实际跑过的“已完成”；构建失败就写失败原因。
