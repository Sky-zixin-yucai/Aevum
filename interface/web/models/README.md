# 项目模型目录

这是 `interface/web` 下的专用模型目录，不新建仓库根目录。

## 结构

```text
models/
├─ generate.js             # 唯一 CLI：按模型名生成，缺省生成全部
├─ gltf/
│  ├─ gltf-builder.js      # 只负责 glTF 2.0 结构：buffer / accessor / mesh / node / material / scene
│  ├─ model-meta.js        # 只负责 Aevum 公共 extras 语义：schema / kind / joints / control / 坐标
│  ├─ gltf-builder.test.js
│  └─ README.md
├─ vehicle/
│  ├─ vehicle-model.js     # 只负责车辆模型语义
│  ├─ vehicle-model.test.js
│  ├─ toy_offroad.gltf
│  └─ README.md
├─ arm/
│  ├─ arm-model.js         # 只负责机械臂模型语义
│  ├─ arm-model.test.js
│  ├─ arm3.gltf
│  └─ README.md
└─ README.md
```

## 生成

```bash
node interface/web/models/generate.js
node interface/web/models/generate.js vehicle
node interface/web/models/generate.js arm
```

## 当前模型

- `vehicle/toy_offroad.gltf`：36 nodes / 8 meshes / 792 triangles / 6 关节。车身、车顶、车窗、灯、保险杠、轮眉、行李架都是静态 part；前轮是 `steer` 枢轴加 `drive` 轮胎，后轮直接 `drive`，四个轮毂跟随轮胎。
- `arm/arm3.gltf`：5 nodes / 5 meshes / 60 triangles。`link1/link2/link3` 是关节，`base/tool` 是静态 part。

## 边界

- 模型文件格式是 **glTF 2.0**；模型参数在 `asset.extras.aevum`。
- `gltf-builder.js` 不认车辆/机械臂；`model-meta.js` 不认具体 kind。
- `vehicle-model.js` 和 `arm-model.js` 不写 buffer/accessor，也不互相引用。
- 生成 CLI 只有一份；同一个 glTF 结构代码只存在一份；同一个公共语义只存在一份。
- 模型文件允许入库，供其他维护者复用；不要放进 `engine/assets/` 或个人临时目录。
