/*
 * 玩具越野车 glTF 模型测试。
 */
"use strict";

const assert = require("assert");
const builder = require("../gltf/gltf-builder.js");
const VehicleModel = require("./vehicle-model.js");

const gltf = VehicleModel.build();
VehicleModel.validate(gltf);

assert.strictEqual(gltf.asset.version, "2.0");
assert.strictEqual(gltf.asset.extras.aevum.kind, "vehicle");
assert.strictEqual(gltf.asset.extras.aevum.placeholder, true);
assert.strictEqual(gltf.asset.extras.aevum.coordinate_system.up, "y");
assert.strictEqual(gltf.meshes.length, 7);
assert.strictEqual(gltf.nodes.length, 30);
assert.strictEqual(gltf.asset.extras.aevum.wheels.length, 4);
assert.strictEqual(gltf.asset.extras.aevum.joints.length, 4);
assert.ok(gltf.asset.extras.aevum.joints.every(function (j) {
  return gltf.asset.extras.aevum.wheels.some(function (w) { return w.id === j.target; });
}));
assert.ok(gltf.buffers[0].uri.startsWith("data:application/octet-stream;base64,"));

const bin = Buffer.from(gltf.buffers[0].uri.split(",")[1], "base64");
assert.strictEqual(bin.length, gltf.buffers[0].byteLength);
gltf.accessors.forEach(function (accessor, index) {
  const view = gltf.bufferViews[accessor.bufferView];
  const comps = accessor.type === "SCALAR" ? 1 : 3;
  const compSize = accessor.componentType === 5123 ? 2 : 4;
  assert.ok(view, "accessor " + index + " bufferView");
  assert.ok((accessor.byteOffset || 0) + accessor.count * comps * compSize <= view.byteLength, "accessor " + index + " overrun");
});

const round = builder.parse(builder.stringify(gltf));
VehicleModel.validate(round);
assert.deepStrictEqual(round, gltf);

const badVersion = JSON.parse(JSON.stringify(gltf));
badVersion.asset.version = "1.0";
assert.throws(function () { VehicleModel.validate(badVersion); }, /glTF 2.0/);

const badWheel = JSON.parse(JSON.stringify(gltf));
badWheel.asset.extras.aevum.wheels[0].node = "missing_wheel";
assert.throws(function () { VehicleModel.validate(badWheel); }, /node/);

console.log("vehicle-model: ok");
console.log(JSON.stringify({
  kind: gltf.asset.extras.aevum.kind,
  nodes: gltf.nodes.length,
  meshes: gltf.meshes.length,
  bufferBytes: gltf.buffers[0].byteLength
}, null, 2));
