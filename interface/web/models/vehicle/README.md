# 玩具越野车 glTF 模型

只负责车辆模型语义：底盘、车厢、四个轮子、转向/驱动关节和通道。

## 文件

- `vehicle-model.js`：车辆数据 + 车辆专属校验；
- `vehicle-model.test.js`：主机测试；
- `toy_offroad.gltf`：生成结果。

## 生成与测试

```bash
node interface/web/models/generate.js vehicle
node interface/web/models/vehicle/vehicle-model.test.js
```

## 格式

标准 glTF 2.0；车辆参数在 `asset.extras.aevum`。公共 glTF 结构和公共 extras 校验分别由 `../gltf/gltf-builder.js` 与 `../gltf/model-meta.js` 负责，这里不复制。

坐标：`x` 向前，`y` 向上，`z` 向右，地面 `x-z`。

当前是占位尺寸，`placeholder: true`；不是实车测量值。

## 外观

- 底盘、车身、引擎盖、车顶、车窗、前后保险杠、格栅、前后灯、侧条纹、轮眉、行李架；
- 前轮是“转向枢轴 + 轮胎”两级：`wheel_fl/fr` 只做 `steer`，子节点 `tire_fl/fr` 做 `drive`；后轮 `wheel_rl/rr` 直接做 `drive`；
- 每个车轮都有独立轮毂 `hub_*`，作为轮胎子节点随转向和滚动一起运动；
- 其余节点是静态 `part`，不产生关节；
- 当前统计：36 nodes / 8 meshes / 792 triangles / 6 关节。

## 不做什么

- 不写 buffer/accessor；
- 不写机械臂；
- 不建 3D 页面；
- 不接内核。
