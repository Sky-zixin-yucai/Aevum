# glTF 公共层

职责唯一，不混模型语义。

## `gltf-builder.js`

只负责 glTF 2.0 结构：

- box / cylinder primitive；
- `pbrMaterial`、`meshNode`、`nodeIndex`；
- buffer、bufferView、accessor；
- mesh、node、material、scene；
- 序列化和结构校验。

测试：

```bash
node interface/web/models/gltf/gltf-builder.test.js
```

不认车辆，也不认机械臂。

## `model-meta.js`

只负责 Aevum 模型 `asset.extras.aevum` 的公共语义：

- `schema`、`kind`、`placeholder`、`units`；
- `coordinate_system`；
- `source`；
- 公共 `joints` 字段和 `control.channels` 引用；
- `sensors`。

不认车辆尺寸，也不认机械臂杆长。kind 专属字段由 `vehicle-model.js` / `arm-model.js` 自己校验。

## 不重复原则

- 几何 primitive 只在这里生成；
- glTF buffer/accessor 只在这里写；
- 公共 extras 校验只在这里写；
- 车辆和机械臂只提供各自的模型数据，不复制公共层。
