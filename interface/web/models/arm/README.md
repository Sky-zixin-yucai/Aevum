# 默认 3R 机械臂 glTF 模型

只负责机械臂模型语义：基座、三节连杆、工具、关节和通道。

## 文件

- `arm-model.js`：机械臂数据 + 机械臂专属校验；
- `arm-model.test.js`：主机测试；
- `arm3.gltf`：生成结果。

## 生成与测试

```bash
node interface/web/models/generate.js arm
node interface/web/models/arm/arm-model.test.js
```

## 格式

标准 glTF 2.0；机械臂参数在 `asset.extras.aevum`。公共 glTF 结构和公共 extras 校验分别由 `../gltf/gltf-builder.js` 与 `../gltf/model-meta.js` 负责，这里不复制。

坐标：`x` 向前，`y` 向上，`z` 向右，地面 `x-z`。

当前是 Forge 3R 默认几何，`placeholder: true`；不是实测尺寸。

## 不做什么

- 不写 buffer/accessor；
- 不写车辆；
- 不建 3D 页面；
- 不接内核。
