/*
 * 默认 3R 机械臂 glTF 模型测试。
 */
"use strict";

const assert = require("assert");
const builder = require("../gltf/gltf-builder.js");
const ArmModel = require("./arm-model.js");

const gltf = ArmModel.build();
ArmModel.validate(gltf);

assert.strictEqual(gltf.asset.version, "2.0");
assert.strictEqual(gltf.asset.extras.aevum.kind, "arm");
assert.strictEqual(gltf.asset.extras.aevum.placeholder, true);
assert.strictEqual(gltf.asset.extras.aevum.coordinate_system.up, "y");
assert.strictEqual(gltf.nodes.length, 5);
assert.strictEqual(gltf.meshes.length, 5);
assert.strictEqual(gltf.asset.extras.aevum.body.links.length, 3);
assert.strictEqual(gltf.asset.extras.aevum.joints.length, 3);
assert.ok(Math.abs(gltf.asset.extras.aevum.joints[1].max - 150 * Math.PI / 180) < 1e-12);
assert.ok(Math.abs(gltf.asset.extras.aevum.joints[0].max - 170 * Math.PI / 180) < 1e-12);
assert.strictEqual(gltf.asset.extras.aevum.body.links[0].dynamics.torque, 12);
assert.ok(gltf.buffers[0].uri.startsWith("data:application/octet-stream;base64,"));

const bin = Buffer.from(gltf.buffers[0].uri.split(",")[1], "base64");
assert.strictEqual(bin.length, gltf.buffers[0].byteLength);

const round = builder.parse(builder.stringify(gltf));
ArmModel.validate(round);
assert.deepStrictEqual(round, gltf);

const badTarget = JSON.parse(JSON.stringify(gltf));
badTarget.asset.extras.aevum.joints[0].target = "missing_link";
assert.throws(function () { ArmModel.validate(badTarget); }, /target/);

const badBase = JSON.parse(JSON.stringify(gltf));
badBase.asset.extras.aevum.body.base.height = -1;
assert.throws(function () { ArmModel.validate(badBase); }, /base\.height/);

console.log("arm-model: ok");
console.log(JSON.stringify({
  kind: gltf.asset.extras.aevum.kind,
  nodes: gltf.nodes.length,
  meshes: gltf.meshes.length,
  bufferBytes: gltf.buffers[0].byteLength
}, null, 2));
