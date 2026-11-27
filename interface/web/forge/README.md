# AEVUM FORGE

页面以内核 3R 平面关节链为唯一控制基座：机械臂是“1”，线、弧、圆都由同一台臂的末端轨迹呈现；模型只是形式，不定义控制指令。切换车辆或导入模型时，控制链仍然是这 3 个关节；车身/整模型作为 3R 末端的外观跟随。内核接管后只允许 `W / A / S / D` 四个键发目标，页面只负责呈现、结构浏览与内核桥接，**不积分、不跑本地 PD、不伪造关节状态**。

## 跑起来

页面必须通过 HTTP 打开，`file://` 下 `fetch` 模型会失败。把仓库根作为静态站点根，打开仓库内的 `/interface/web/forge/` 即可；不要在 README 里绑定某台机器的端口或绝对目录。

顶部 `MODEL` 可在 `3R ARM / TOY OFFROAD` 之间切换，`Import glTF` 可载入本地 `.gltf` / `.glb`。所有模型共用同一套 3R 控制链；切换不会改变 body 的 `n=3`。

页面不依赖 React、Three.js、构建工具或外部素材。3D 视口、矩阵、相机、网格、关节标签和绘制顺序都在 `js/render/`。

## 界面

单一工作台：

- 左侧 `STRUCTURE / Outliner` 显示模型节点树。`P` 是静态 part，`J` 是模型声明的关节。
- 右侧 `PROPERTIES / Inspector` 显示模型事实、节点角色、关节通道、内核状态。未接内核时关节角度只读。
- 顶部 `MODEL` 切换 `3R ARM / TOY OFFROAD`，`Import glTF` 载入本地模型；模型只改变呈现形式，不改变控制链。
- 内核接管后只用 `W / A / S / D`：四键相互独立，`W` 只前进、`S` 只后退、`A/D` 只打左/右前轮；前轮角度会保留到下一次 `W/S`，先打轮再给油就按该角度走弧。实时运动由驾驶位姿积分并继续向内核下发 `q_target`，实际关节角由 `0x03` 回传。
- 顶部 `VIEW` 下拉直接切换汽车相机：`任意` / `固定跟随` / `跟随` / `第一人称`，也可按 `V` 循环。`任意` 是自由环绕；`固定跟随` 在当前机位记下相对车体的偏移，之后相机跟着车保持这个机位；`跟随` 是车后跟随第三人称；`第一人称` 是车头视角。相机复用 `render/scene/camera.js`，不在页面里另做一套相机。
- 车辆/导入模型按游戏式控制：`W/S` 是油门前进/后退，`A/D` 是方向盘打前轮；四键互斥，按住 `A/D` 时 `W/S` 不响应，行驶中按 `A/D` 也不响应，必须松开一组再按另一组。前轮角度会保留，先 `A/D` 打轮、松开后再 `W/S`，车就按这个角度走弧；松开 `W/S` 后 `A/D` 可以继续调整角度。相机不锁在车体上，车会在场景里真正位移，而不是永远停在画面中央。
- 视口右上有 `KERNEL DEBUG`，实时显示 body、下发的 J、0x03 q_target/q_actual、target 与帧计数；不把关键状态藏在浏览器控制台。
- 视口左键 orbit、中键 pan、滚轮 zoom；`F` frame、`G` grid、`O` ortho、`1/3/7/0` front/right/top/home。
- 底部 Telemetry 默认显示模型节点、关节、三角形与网格数；内核接入后切换为 `0x03` 状态。

## 模型语义

源模型格式是 **glTF 2.0**，不是自定义 JSON。

- 几何、节点、材质、buffer 都在 glTF 内。
- 公共语义在 `asset.extras.aevum`，节点角色在 `node.extras.aevum`。
- 只有 `asset.extras.aevum.joints[].target` 指向的节点被加载器标为 `actuated`。
- 其它节点是静态 part：底盘、车身、车窗、灯、保险杠、工具外壳；它们随父节点变换，但不产生关节、不接受目标。
- 车辆视图默认只画关节标签；选中关节时才画关节环。转向枢轴下的滚动子关节不重复画环，避免前轮出现两套圆环。
- 加载器保留 `modelKind`、`aevum`、`role`、`semanticId`、`jointType`、`channel`、限位与 `continuous`。

已入库模型：

| key | 文件 | 内容 |
| --- | --- | --- |
| `toy_offroad` | `interface/web/models/vehicle/toy_offroad.gltf` | 3R 外观：36 nodes / 8 meshes / 792 tris / 6 车轮关节；车身位姿由 3R 末端推导，四轮按位移滚转，前轮按 `A/D` 打角 |
| `arm3` | `interface/web/models/arm/arm3.gltf` | 固定 3R 基座：5 nodes / 5 meshes / 60 tris / 3 语义关节 |

车辆参数（`asset.extras.aevum`）：车长 0.32、车宽 0.18、车高 0.10、轴距 0.22、轮距 0.18、离地间隙 0.03、轮半径 0.04、轮宽 0.025、质量 0.8。前轮 `steer` + `drive` 两级，后轮 `drive`，四个轮毂随轮胎运动，控制周期 100 Hz。

外观件全部在 `vehicle-model.js` 里生成：车身、车顶、车窗、保险杠、格栅、前后灯、侧条纹、轮眉、车顶行李架和四个轮毂。改外观不改加载器和页面。

生成与测试：

```bash
node interface/web/models/generate.js
node interface/web/models/gltf/gltf-builder.test.js
node interface/web/models/vehicle/vehicle-model.test.js
node interface/web/models/arm/arm-model.test.js
```

只生成一个模型时，在 `generate.js` 后面加 `vehicle` 或 `arm`。

## 控制边界

页面没有任何本地控制路径：

- 没有 `pd-loop.js`、`demo-motion.js`、`telemetry.js`、`controls.js`。
- 没有任何模型专用按钮：`Run LINE / Run ARC / Full Circle` 和单关节滑杆已删除。
- 帧循环只做绘制、相机、遥测、FPS 和内核状态更新，不调用控制 `tick`。
- `W / A / S / D` 是唯一的内核输入：页面用平面雅可比把一次按键的末端位移映射成 `q_target`，内核负责限幅、速度、PD 和 5 秒保持窗口；页面不积分。
- 桥接断开时页面退回模型查看，不宣称“本地仿真继续”。

控制权在内核。页面只做参数下发、呈现和记录。

## 为什么之前“按了没动”

`arm3.gltf` 是 5 个平铺节点：`base / link1 / link2 / link3 / tool`，每个 node 的 `translation` 都是世界坐标，没有 `children` 父子链。旧页面把“5 个几何节点”当成“5 段机械臂”：

- 旧 `kernelCompatibility()` 要求单链 parent，而 5 个节点全是 `parent=-1`，所以 body descriptor 根本没有发给内核；
- 页面没有 `0x03` 的 q 写回，模型当然不动；
- QEMU 里的内核机械臂继续跑自己的 AUTO，页面和内核各画各的，看起来就是“控制和演示没有关系”。

现在的处理是：

- 控制链只取 `aevum.body.links` 声明的 3 个语义关节（`J1/J2/J3`），不取 base/tool；
- 杆长取 `body.links[].length`（0.62 / 0.52 / 0.24 m），不取单位盒的 mesh 包围盒；
- 渲染按 `body.links` 重算世界矩阵，q 变化时整条链真正铰接；
- body descriptor 是 `n=3`，`0x03` 的 3 个 q 写回这 3 个关节；
- `W/A/S/D` 每次只产生一个末端目标增量，`J` 命令下发后由内核 PD 执行。

## 调试面板读什么

`KERNEL DEBUG` 每次刷新把这些方向分开显示：

```text
BODY→K  n=3  L=0.620 / 0.520 / 0.240 m   # 页面下发的 body descriptor
PAGE→K  J q_target=... rad               # 页面刚发给内核的关节目标
K→PAGE  0x03 n=3 frame=... mode=MANUAL   # 内核回传的状态帧
        q_target=...°  q_actual=...°      # 目标与真实角度，二者应逐步收敛
FORGE   q=...°  tgt=...°                 # 页面当前显示，必须来自 0x03
```

如果 `PAGE→K` 有 J、`K→PAGE` 的 `q_actual` 不动，问题在内核或桥接；如果 `K→PAGE` 在动而模型不动，问题在渲染映射。两者不再混在一起。

## 内核桥接

```text
QEMU COM1 (TCP 5555)
  ├─ A5 5A 03 COUNT + COUNT×i32 LE + CRC   # COUNT = 3n+10
  │    └─ serial_bridge.py → ws://127.0.0.1:8765 {type:"status", n, q, dq, qTarget, values}
  │         └─ Forge  KERNEL / MANUAL HOLD | AUTO
  ├─ 'B' + u8 n + n×4×i32 LE               # ← Forge {type:"body", body:{n,lengths,qMin,qMax,qHome}}
  └─ 'J' + u8 n + n×int16 LE (0.001 rad)   # ← Forge {type:"joint", q}
```

启动顺序见 [`scripts/kernel/README.md`](../../../scripts/kernel/README.md)。页面默认连接 `ws://127.0.0.1:8765`，也可在 Inspector 的 `Kernel bridge` 里改 URL。

当前内核只接受平面串联链：2–8 个语义关节、单杆 ≤ 2 m、总 reach ≤ 16 m。`arm3` 通过 `aevum.body.links` 得到 3 关节 / 1.38 m reach，可以接内核；`toy_offroad` 的 6 个车轮关节不进入 body descriptor，页面固定用 3R 的 3 个 q 推导整车位姿，不再给车辆单独路径按钮。

`body descriptor` 只在连接、换文档、改限位时下发。手动目标有 5 秒保持窗口，页面每 2 秒重发；桥接断开时内核自行回 AUTO。

## 公开 API

```js
window.AEVUM_FORGE
```

| 方法 | 作用 |
| --- | --- |
| `getState()` | 模型类型、kernel/manual、q/target、选中项、bridge、BodyDocument 快照 |
| `getDocument()` | 复制当前内部文档 |
| `setDocument(doc)` | 校验并替换文档 |
| `importModel(name, buffer)` | 解析 `.glb` / `.gltf` ArrayBuffer，返回 `modelKind` / `nodes` / `triangles` / kernel 兼容性 |
| `setTarget(index, degrees)` | 仅在内核接管且兼容时下发单关节目标；否则抛错 |
| `setTargets(degrees[])` | 批量下发；同样要求内核接管 |
| `setManual(on)` | 内核下开 / 关手动保持；关闭时下发 `R` |
| `getTelemetry()` | 内核状态或模型统计；不返回本地仿真指标 |
| `connect(url)` / `disconnect()` | 连 / 断 WebSocket |
| `subscribe(fn)` | 订阅遥测事件，返回取消函数 |

`AEVUM_FORGE.viewOnly === true`。页面不提供 `run / pause / reset / sine / sweep / grab` 这类本地控制入口。

## 目录

```text
forge/
├─ index.html                  # 布局 + CSS/JS 引用
├─ api.js                      # window.AEVUM_FORGE
├─ app.js                      # 组合根：模型加载、UI 接线、帧循环
├─ styles/                     # tokens / base / shell / stage / panels / telemetry
└─ js/
   ├─ core/                    # namespace / math / dom
   ├─ app/
   │  ├─ workbench.js          # 视口交互、ViewCube、面板折叠、快捷键
   │  ├─ bridge-control.js     # WebSocket、0x03 状态、body/joint 命令
   │  ├─ keyboard-control.js   # W/A/S/D 末端目标 → q_target
   │  ├─ api-handlers.js       # api.js 背后的实现
   │  └─ frame-loop.js         # 只绘制/遥测，不控制
   ├─ model/
   │  ├─ body-document.js      # 内部 BodyDocument 与 kernelCompatibility
   │  ├─ gltf-loader.js        # glTF 2.0 → 文档，保留 aevum/actuated
   │  └─ catalog.js            # 模型文件索引；仅供加载器保留，页面不再切换
   ├─ control/
   │  └─ session.js            # 最小会话状态，无积分
   ├─ render/                  # camera / arm-scene / body-scene / labels / renderer
   ├─ ui/                      # tree / inspector / status / telemetry / debug-panel / toast
   └─ bridge/websocket.js
```

规则：

1. `index.html` 不写业务逻辑，只装配。
2. `api.js` 不碰 DOM、不碰渲染。
3. `app.js` 只做组合，不实现矩阵、mesh 或控制公式。
4. 一个文件一个职责；新增能力先找已有 owner，不复制一份。
5. 页面不放在 `build/`。

## 边界

`interface/web/forge/` 只做呈现与内核桥接可视化，不直接读数据库、不直接碰串口，也不承载身份业务。串口桥和内核分别在 `src/kernel/host/bridge/` 与 `src/kernel/host/console/robot/`。

3D 世界可以当内核的被控对象，不能当内核的替身。没有内核状态帧时，页面只显示模型，不推算车辆运动。
