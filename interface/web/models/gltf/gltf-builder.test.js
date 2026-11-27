/*
 * glTF 公共结构测试。只测结构，不测车辆或机械臂。
 */
"use strict";

const assert = require("assert");
const builder = require("./gltf-builder.js");

assert.strictEqual(builder.FORMAT, "glTF 2.0");

const box = builder.boxGeometry();
assert.strictEqual(box.positions.length, 24 * 3);
assert.strictEqual(box.normals.length, 24 * 3);
assert.strictEqual(box.indices.length, 36);

const cylinder = builder.cylinderGeometry(8);
assert.strictEqual(cylinder.positions.length, (8 * 4 + 6) * 3);
assert.strictEqual(cylinder.indices.length, 8 * 6 + 8 * 3 + 8 * 3);

const material = builder.pbrMaterial("paint", [0.2, 0.4, 0.6], 0.7);
assert.strictEqual(material.name, "paint");
assert.deepStrictEqual(material.pbrMetallicRoughness.baseColorFactor, [0.2, 0.4, 0.6, 1]);
assert.strictEqual(material.pbrMetallicRoughness.roughnessFactor, 0.7);
assert.throws(function () { builder.pbrMaterial("bad", [1, 2]); }, /color/);

const node = builder.meshNode({ name: "only_mesh", mesh: 0 });
assert.deepStrictEqual(node, { name: "only_mesh", mesh: 0 });

const gltf = {
  nodes: [{ name: "a" }, { name: "b" }],
  meshes: [],
  materials: [],
  buffers: [],
  scenes: [{ nodes: [0, 1] }],
  scene: 0
};
assert.strictEqual(builder.nodeIndex(gltf, "b"), 1);
assert.strictEqual(builder.nodeIndex(gltf, "missing"), -1);

const built = builder.buildModel({
  name: "TEST",
  parts: [{ name: "box", geometry: box, material: 0 }],
  nodes: [builder.meshNode({ name: "box", mesh: 0, extras: { aevum: { role: "test" } } })],
  materials: [builder.pbrMaterial("box", [1, 1, 1], 1)],
  aevum: {}
});
assert.strictEqual(built.meshes.length, 1);
assert.strictEqual(built.buffers[0].byteLength, Buffer.from(built.buffers[0].uri.split(",")[1], "base64").length);
assert.deepStrictEqual(builder.parse(builder.stringify(built)), built);

console.log("gltf-builder: ok");
