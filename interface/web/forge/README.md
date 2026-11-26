# AEVUM FORGE

机器人专用 3D 建模 / 控制台。第一版内核支持由宿主描述驱动的 **2–8 关节平面串联链**，默认 3R；同时支持导入 glTF / GLB，把任意节点树变成可编辑 link/joint 树。内核接入后可按关节手动控制，并显示完整运动学状态。

## 跑起来

直接打开：

`interface/web/forge/index.html`

或从 `interface/web` 起静态服务：

```bash
cd interface/web
python -m http.server 8080
```

打开 `http://localhost:8080/forge/`。

页面不依赖 React、Three.js、构建工具或外部素材。3D 视口、矩阵、相机、网格、关节标签和绘制顺序都在 `js/render/` 下，由页面自己的 WebGL 代码实现。

## 工作台界面

页面是单一工作台，不切开多个 workspace。常用操作直接长在视口里：左键 orbit、中键 pan、滚轮 zoom；`F` frame、`G` grid、`O` ortho、`1/3/7/0` front/right/top/home。视口右上 ViewCube 可切视图，左上 HUD 显示当前文档与关键指标；左右面板可用 `‹` / `›` 折叠，底部 Motion Telemetry 可 Expand / Collapse。

- 左侧 `STRUCTURE / Outliner` 管 link/joint 树；搜索框过滤节点。
- 右侧 `PROPERTIES / Inspector` 只显示当前选中对象的属性；`KERNEL STATUS` 只在接上内核后出现。
- 顶部 `BODY` 预设可切 2R–8R 平面链；切换时页面把 `body descriptor`（关节数、杆长、限位、home）下发给内核，不需要重编译。`Import` / `Save` / `More` 是文档级动作；`Sine` 收在 More，避免常驻。
- 导入模型若不是 2–8 关节平面链（单链 parent、Z 轴、单杆 ≤ 2 m、总 reach ≤ 16 m 之外），显示 `KERNEL LOCAL ONLY`，仍可本地建模和 PD。
- `SWEEP` 只对 J1 叠加约 ±37°、0.4 Hz 的正弦目标，用来演示左右自转；`GRAB` 只服务默认 3R：场景里有一个 `doc.prop` 方块，宿主用 3R IK 把工具尖端对到方块左侧面，再闭合夹爪。两者都是宿主侧参数实例，内核只收到普通 `J` 和 `R`。
- `GRAB` 在方块被夹住后按钮变成 `RELEASE`；2R、4R–8R 或没有 `doc.prop` 时不启用。方块和夹爪是 Forge 渲染层的演示对象，不属于 `body_desc`。
- 4K 下根字号、面板宽度、视口控件按视口宽度提升；默认按 2560–3840 的展示密度排。

## 建模与导入

- 左侧 `STRUCTURE / Outliner` 显示当前 link/joint 树。每个节点可选中，右侧 Inspector 编辑关节轴、原点、目标角、限位、力矩、惯量和 PD 参数。
- 顶部 `Import` 接受 `.glb`，或同时选择 `.gltf` 与它引用的 `.bin`。导入后自动生成 `aevum.body/1`：每个 glTF node 变成一个 link，父子节点自动生成 joint；轴优先读 `node.extras.jointAxis`，否则按节点平移主轴启发式推断。仓库自带 `samples/arm3.glb`，3 links / 36 triangles，可直接导入验证。
- 导入模型按节点自动建 link/joint 树。只要形成 2–8 关节的平面串联链（Z 轴、单杆 ≤ 2 m、总 reach ≤ 16 m），就能把 body descriptor 下发给内核并接收状态；几何不再要求固定 0.30/0.30/0.20 m。其他模型显示 `KERNEL LOCAL ONLY`，不会被内核状态覆盖。
- `Save` / API `exportJson()` 保存完整 `aevum.body/1`，包含三角网格；导入模型暂不导出 MuJoCo XML。

## 接真实内核（手动关节 + 完整状态）

链路是双向的：

```text
QEMU COM1 (TCP 5555)
  ├─ A5 5A 03 COUNT + COUNT×i32 LE + CRC   # COUNT = 3n+10；n=3→19，n=8→34
  │    └─ serial_bridge.py → ws://127.0.0.1:8765 {type:"status", n, q, dq, qTarget, values}
  │         └─ Forge  KERNEL / MANUAL HOLD | AUTO
  ├─ 'B' + u8 n + n×4×i32 LE               # ← Forge {type:"body", body:{n,lengths,qMin,qMax,qHome}}
  └─ 'J' + u8 n + n×int16 LE (0.001 rad)   # ← Forge {type:"joint", q}
```

启动顺序（脚本和依赖说明见 [`scripts/kernel/README.md`](../../../scripts/kernel/README.md)）：

1. 构建内核。Windows 用 WSL 的默认发行版，或通过 `-Distro` / `AEVUM_WSL_DISTRO` 指定：

```powershell
.\scripts\kernel\build-kernel.ps1
```

Linux / WSL 内：

```bash
./scripts/kernel/build-kernel.sh
```

2. 启动 QEMU。脚本从仓库根目录定位 `disk.img`，并固定使用 TCP 串口：

```powershell
.\scripts\kernel\run-qemu.ps1
```

3. 启动 WebSocket 桥接：

```powershell
.\scripts\kernel\run-bridge.ps1
```

Linux / WSL 对应使用 `./scripts/kernel/run-qemu.sh` 和 `./scripts/kernel/run-bridge.sh`。QEMU 或 Python 不在 `PATH` 时，用 `AEVUM_QEMU` / `AEVUM_PYTHON` 覆盖；不需要把本机路径写进文档。

4. 打开 `interface/web/forge/index.html`，用公开 API 连接：

```js
AEVUM_FORGE.connect();
```

进入内核模式的标志：

- 顶部变成 `KERNEL LINK`，模式 pill 显示 `KERNEL / MANUAL HOLD` 或 `KERNEL / AUTO`；
- `RUN` 按钮变成 `MANUAL` / `MANUAL ON`；`Sine` 和 `Reset` 禁用，本地 100 Hz PD 暂停；
- Inspector 的 `KERNEL STATUS / 0x03` 显示 MODE、末端误差、可操作度、sigma min/max、null-space share、limit margin、max |dq|、target X/Y；
- 底部曲线切换成内核 q 轨迹；拖动 target 或调用 `setTargets()` 会下发 `J` 并进入手动保持；
- 手动保持由页面按桥接 100 ms 节流重发 `joint`；关掉 MANUAL 或调用 `setManual(false)` 会立即下发 `R`，内核马上回 AUTO；桥接断开或命令丢失时，内核 5 秒无命令兜底。
- 内核 AUTO 时状态帧的 `q_target` 跟随当前 q，手动入口不会跳到上一次手动目标；状态帧按渲染帧节流发送，避免串口被状态帧占满。

边界：

- 第一版只覆盖平面串联链：2–8 关节、单链 parent、Z 轴、单杆 ≤ 2 m、总 reach ≤ 16 m；3D 链、分支、移动底盘和夹爪仍是后续范围。
- body descriptor 由页面在连接、换文档、改杆长/限位时下发；内核按收到的描述重置关节与 FK。默认 3R 是 `0.30/0.30/0.20 m`，Forge 原生 3R 文档约 1.58 m reach，因此连接后以内核收到的描述为准。
- 手动模式是关节空间 PD，不替代内核原有自动正弦/雅可比控制；超时回 AUTO 由内核 PIT tick 判定。


## 目录约定

浅层只放入口和接口，深层放实现：

```text
forge/
├─ index.html                  # 只做装配：布局 + CSS/JS 引用
├─ api.js                      # 只做公开 API：window.AEVUM_FORGE
├─ app.js                      # 组合根：state、模块接线、帧循环
├─ styles/
│  ├─ tokens.css               # 颜色/尺寸令牌
│  ├─ base.css                 # reset + 基础控件
│  ├─ shell.css                # header / 三栏骨架 / toast
│  ├─ stage.css                # 3D 视口 / HUD / 关节标签
│  ├─ panels.css               # 左树 / 右 Inspector / bridge 表单
│  └─ telemetry.css            # footer 指标 / 误差曲线
└─ js/
   ├─ core/
   │  ├─ namespace.js          # AEVUM.FORGE 命名空间
   │  ├─ math.js               # 矩阵 / 投影 / 角度 / 数值格式
   │  └─ dom.js                # DOM 查询 / 下载 / 文件读取
   ├─ app/
   │  ├─ workbench.js          # 视口 orbit/pan/zoom、ViewCube、面板折叠、快捷键
   │  ├─ bridge-control.js     # WebSocket 生命周期 / 动态 0x03 状态 / body + joint 命令
   │  ├─ api-handlers.js       # api.js 背后的命令实现
   │  └─ frame-loop.js         # 帧循环：绘制 → 控制 → 遥测
   ├─ model/
   │  ├─ body-document.js      # BodyDocument schema / 2–8R 预设与导入校验 / body descriptor
   │  ├─ gltf-loader.js        # GLB / glTF 2.0 → aevum.body/1
   │  └─ exporters.js          # JSON / MuJoCo XML / virtual-bus manifest
   ├─ control/
   │  ├─ pd-loop.js            # 100 Hz PD 被控对象
   │  └─ telemetry.js          # RMS / peak / torque 只读指标
   ├─ render/
   │  ├─ gl/
   │  │  ├─ shaders.js         # GLSL
   │  │  ├─ meshes.js          # box / cylinder / grid 顶点数据
   │  │  ├─ context.js         # WebGL context + program
   │  │  └─ buffers.js         # VBO 上传 / 任意三角网格 / draw calls
   │  ├─ scene/
   │  │  ├─ camera.js          # 相机状态 + view-projection + 导入自动取景
   │  │  ├─ arm-scene.js       # 原生平面链 → 世界变换
   │  │  ├─ body-scene.js      # 任意 link/joint 树 → 世界变换
   │  │  └─ joint-labels.js    # 关节标签投影
   │  └─ viewport/
   │     └─ renderer.js        # 绘制顺序 / 清屏 / 地面 / 工具
   ├─ ui/
   │  ├─ toast.js
   │  ├─ document-tree.js      # 左侧 BODY DOCUMENT
   │  ├─ inspector.js          # 右侧参数编辑 / KERNEL STATUS
   │  ├─ controls.js           # RUN / MANUAL / SINE / RESET
   │  └─ telemetry-panel.js    # footer 指标 + 曲线
   └─ bridge/
      └─ websocket.js          # WebSocket 接缝，不认识内核
```

规则：

1. `index.html` 不写业务逻辑，只装配。
2. `api.js` 不碰 DOM、不碰渲染，只转发命令和快照。
3. `app.js` 只做组合，不实现矩阵、mesh、PD 公式。
4. 越底层的代码放越深；例如 GLSL 在 `render/gl/`，场景变换在 `render/scene/`。
5. 一个文件一个职责；新增能力先找已有 owner，不复制一份。

## 公开 API

页面加载后暴露：

```js
window.AEVUM_FORGE
```

可用方法：

| 方法 | 作用 |
| --- | --- |
| `getState()` | 当前运行状态、q/target、选中项、bridge、`kernel` / `manual` / `kernelStatus`、BodyDocument 快照 |
| `getDocument()` | 复制当前 BodyDocument |
| `setDocument(doc)` | 校验并替换 BodyDocument，重置仿真 |
| `importModel(name, buffer)` | 解析 `.glb` 或 `.gltf` ArrayBuffer，替换为 `aevum.body/1`，返回 segments / triangles / kernel 兼容性 |
| `setTarget(index, degrees)` | 设置单个关节目标角；kernel 下自动进入手动并下发 |
| `setTargets(degrees[])` | 批量设置目标角；kernel 下自动进入手动并下发 |
| `setManual(on)` | kernel 下开 / 关手动保持；关闭时下发 `R`，内核立即回 AUTO，5 秒超时仅作兜底 |
| `sweep(on)` | 开 / 关 J1 左右自转演示；内核下持续下发 `J`，本地走 PD |
| `grab()` | 默认 3R 方块抓取；已夹住时再次调用等价于 `releaseGrip()` |
| `releaseGrip()` | 松开夹爪并把方块放回原位 |
| `run()` / `pause()` | 本地启动 / 暂停 100 Hz PD；kernel 下等价于开 / 关手动保持 |
| `reset()` | 回到目标姿态（仅本地；kernel 抛错） |
| `sine(on)` | 开 / 关正弦激励；开时自动启动（仅本地；kernel 抛错） |
| `getTelemetry()` | 本地 RMS / 峰值 / 力矩 / 丢包；kernel 下返回 q/dq/qTarget/末端误差/可操作度/sigma/null share/limit margin/mode |
| `exportJson()` / `exportXml()` / `exportManifest()` | 导出文档 / MuJoCo 模型 / 虚拟总线 manifest |
| `connect(url)` / `disconnect()` | 连 / 断 WebSocket 接缝 |
| `subscribe(fn)` | 订阅遥测事件，返回取消函数 |

示例：

```js
AEVUM_FORGE.setTarget(1, 20);
AEVUM_FORGE.run();

const stop = AEVUM_FORGE.subscribe(function (msg) {
  if (msg.type === "telemetry") console.log(msg.rms, msg.peak);
});
```

## 内核边界

内核不在这里被 `import`。它是独立二进制，由 QEMU / Renode / 真实板加载；页面只负责呈现和本地控制可视化。

右上角 `KERNEL OFFLINE` 是真实状态，不是装饰。当前已接通的接缝：

```text
kernel ↔ UART / virtual bus ↔ WebSocket bridge ↔ interface/web/forge
```

P6 内核 UART 现在发两类帧，并接受四种输入：

- `A5 5A 01 COUNT [row col ch attr] * COUNT CRC`：VGA 单元差分；
- `A5 5A 03 COUNT + COUNT×i32 LE + CRC`：动态状态，`COUNT = 3n + 10`；字段顺序是 q n / dq n / q_target n / target.x,y / err_mm / w_manip / sigma_min / sigma_max / null_share / limit_margin / mode / null_on；
- `B` + u8 n + n×4×i32 LE（Q16.16）：body descriptor，顺序 length/min/max/home；
- `J` + u8 n + n×int16 LE（0.001 rad）：页面下发关节目标；
- `R`：释放手动保持，内核立即回 AUTO；
- `F`：请求内核全量 VGA 刷新。

桥接把 `0x03` 转成 `{type:"status", n, q, dq, qTarget, values}` 广播，新客户端会收到最近一帧；页面消息 `{type:"body", body}` 转成 `B`，`{type:"joint", q}` 转成 `J`，`{type:"release"}` 转成 `R`。内核手动模式仍保留 PIT `MANUAL_TIMEOUT_TICKS=500`（100 Hz，5 秒）作为桥接断开时的兜底。

页面默认连接 `ws://127.0.0.1:8765`。非 2–8 关节平面链会被 `kernelCompatibility()` 拦下，只做本地建模，不会污染 q。

## 目录边界

`interface/web/forge/` 只做呈现与本地控制可视化，不直接读数据库、不直接碰串口，也不承载身份业务。真正的串口桥和内核分别在 `src/kernel/host/bridge/` 与 `src/kernel/host/console/robot/`。

Forge 不放在 `build/`：`build/` 是构建产物和本地缓存，`.gitignore` 会忽略它；源码和产品页面放在 `interface/web/forge/`。
