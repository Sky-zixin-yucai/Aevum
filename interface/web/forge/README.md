# AEVUM FORGE

通用 glTF 模型工作台。页面默认加载玩具越野车 `toy_offroad.gltf`，可切换 3R 机械臂或导入任意 `.gltf` / `.glb`；页面只负责呈现、结构浏览与内核桥接，**不积分、不跑本地 PD、不伪造关节状态**。

## 跑起来

页面必须通过 HTTP 打开，`file://` 下 `fetch` 模型会失败：

```bash
cd interface/web
python -m http.server 8080
```

打开 `http://localhost:8080/forge/`。可用 `?model=toy_offroad` 或 `?model=arm3` 指定初始模型。

页面不依赖 React、Three.js、构建工具或外部素材。3D 视口、矩阵、相机、网格、关节标签和绘制顺序都在 `js/render/`。

## 界面

单一工作台：

- 左侧 `STRUCTURE / Outliner` 显示模型节点树。`P` 是静态 part，`J` 是模型声明的关节。
- 右侧 `PROPERTIES / Inspector` 显示模型事实、节点角色、关节通道、内核状态。未接内核时关节角度只读。
- 顶部 `MODEL` 选择已入库模型，`Import glTF` 可载入 `.gltf` / `.glb`。
- 视口左键 orbit、中键 pan、滚轮 zoom；`F` frame、`G` grid、`O` ortho、`1/3/7/0` front/right/top/home。
- 底部 Telemetry 默认显示模型节点、关节、三角形与网格数；内核接入后切换为 `0x03` 状态。

## 模型语义

源模型格式是 **glTF 2.0**，不是自定义 JSON。

- 几何、节点、材质、buffer 都在 glTF 内。
- 公共语义在 `asset.extras.aevum`，节点角色在 `node.extras.aevum`。
- 只有 `asset.extras.aevum.joints[].target` 指向的节点被加载器标为 `actuated`。
- 其它节点是静态 part：底盘、车身、车窗、灯、保险杠、工具外壳；它们随父节点变换，但不产生关节、不接受目标。
- 加载器保留 `modelKind`、`aevum`、`role`、`semanticId`、`jointType`、`channel`、限位与 `continuous`。

已入库模型：

| key | 文件 | 内容 |
| --- | --- | --- |
| `toy_offroad` | `interface/web/models/vehicle/toy_offroad.gltf` | 30 nodes / 7 meshes / 600 tris / 4 轮关节 |
| `arm3` | `interface/web/models/arm/arm3.gltf` | 5 nodes / 5 meshes / 60 tris / 3 关节 |

车辆参数（`asset.extras.aevum`）：车长 0.32、车宽 0.18、车高 0.10、轴距 0.22、轮距 0.18、离地间隙 0.03、轮半径 0.04、轮宽 0.025、质量 0.8。前轮 `steer`，后轮 `drive`，控制周期 100 Hz。

外观件全部在 `vehicle-model.js` 里生成：车身、车顶、车窗、保险杠、格栅、前后灯、侧条纹、轮眉、车顶行李架。改外观不改加载器和页面。

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
- 没有 `RUN / SWEEP / GRAB / SINE / Reset`，没有 `BODY 2R–8R` 预设。
- 帧循环只做绘制、相机、遥测、FPS 和内核状态更新，不调用控制 `tick`。
- `Inspector` 的目标滑块只在**内核已接管且模型兼容**时可拖动；拖动只下发 `q_target`，显示角度来自内核 `0x03`。
- 桥接断开时页面退回模型查看，不宣称“本地仿真继续”。

控制权在内核。页面只做参数下发、呈现和记录。

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

当前内核只接受平面串联链：2–8 个关节、单链 parent、Z 轴、单杆 ≤ 2 m、总 reach ≤ 16 m。`toy_offroad` 是 30 节点、4 轮分支，`arm3` 是平铺节点，二者都显示 `MODEL VIEW`，不会被内核状态污染。符合该契约的模型导入后才可下发 body descriptor。

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
   │  ├─ api-handlers.js       # api.js 背后的实现
   │  └─ frame-loop.js         # 只绘制/遥测，不控制
   ├─ model/
   │  ├─ body-document.js      # 内部 BodyDocument 与 kernelCompatibility
   │  ├─ gltf-loader.js        # glTF 2.0 → 文档，保留 aevum/actuated
   │  └─ catalog.js            # 默认 toy_offroad / arm3 与 URL 加载
   ├─ control/
   │  └─ session.js            # 最小会话状态，无积分
   ├─ render/                  # camera / arm-scene / body-scene / labels / renderer
   ├─ ui/                      # tree / inspector / status / telemetry / toast
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
