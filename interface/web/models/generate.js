/*
 * 模型生成 CLI：同一套流程生成车辆与机械臂。
 *
 * 只做参数选择、调用模型模块、写文件和打印摘要。
 * 不认几何、不认关节、不认 kind 专属字段。
 */
"use strict";

const fs = require("fs");
const path = require("path");
const builder = require("./gltf/gltf-builder.js");

const MODELS = {
  vehicle: { module: "./vehicle/vehicle-model.js", output: "vehicle/toy_offroad.gltf" },
  arm: { module: "./arm/arm-model.js", output: "arm/arm3.gltf" }
};

const requested = process.argv.slice(2);
const names = requested.length ? requested : Object.keys(MODELS);

names.forEach(function (name) {
  const entry = MODELS[name];
  if (!entry) throw new Error("未知模型: " + name + "；可用: " + Object.keys(MODELS).join(" / "));

  const Model = require(entry.module);
  const gltf = Model.build();
  const outputPath = path.join(__dirname, entry.output);

  fs.writeFileSync(outputPath, builder.stringify(gltf), "utf8");
  console.log("wrote " + outputPath);
  console.log(JSON.stringify({
    format: builder.FORMAT,
    kind: gltf.asset.extras.aevum.kind,
    nodes: gltf.nodes.length,
    meshes: gltf.meshes.length,
    bufferBytes: gltf.buffers[0].byteLength,
    placeholder: gltf.asset.extras.aevum.placeholder
  }, null, 2));
});
