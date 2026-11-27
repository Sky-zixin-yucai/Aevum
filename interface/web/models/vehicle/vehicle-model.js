/*
 * 职责唯一：只负责玩具越野车的 glTF 模型语义。
 *
 * glTF 结构由 gltf-builder.js 负责，公共 extras 语义由 model-meta.js 负责。
 * 本文件不写 buffer/accessor，也不处理机械臂。
 */
"use strict";

const builder = require("../gltf/gltf-builder.js");
const meta = require("../gltf/model-meta.js");

const DIMENSIONS = {
  length: 0.32,
  width: 0.18,
  height: 0.10,
  wheelbase: 0.22,
  track: 0.18,
  ground_clearance: 0.03,
  wheel_radius: 0.04,
  wheel_width: 0.025,
  mass: 0.8
};

const WHEEL_COLOR = [0.11, 0.12, 0.13];
const CHASSIS_COLOR = [0.22, 0.26, 0.20];
const CABIN_COLOR = [0.30, 0.36, 0.28];

function build() {
  const dim = Object.assign({}, DIMENSIONS);
  const halfTrack = dim.track / 2;
  const frontX = dim.wheelbase / 2;
  const rearX = -dim.wheelbase / 2;
  const wheelY = dim.wheel_radius;
  const bodyY = dim.ground_clearance + dim.height / 2;
  const cabinHeight = dim.height * 0.35;
  const cabinY = dim.ground_clearance + dim.height + cabinHeight / 2;

  const wheels = [
    { id: "FL", node: "wheel_fl", pos: [frontX, wheelY, halfTrack], radius: dim.wheel_radius, width: dim.wheel_width },
    { id: "FR", node: "wheel_fr", pos: [frontX, wheelY, -halfTrack], radius: dim.wheel_radius, width: dim.wheel_width },
    { id: "RL", node: "wheel_rl", pos: [rearX, wheelY, halfTrack], radius: dim.wheel_radius, width: dim.wheel_width },
    { id: "RR", node: "wheel_rr", pos: [rearX, wheelY, -halfTrack], radius: dim.wheel_radius, width: dim.wheel_width }
  ];

  const joints = [
    { id: "steer_fl", type: "steer", parent: "chassis", target: "FL", axis: [0, 1, 0], min: -0.6, max: 0.6, home: 0, continuous: false, channel: "steer" },
    { id: "steer_fr", type: "steer", parent: "chassis", target: "FR", axis: [0, 1, 0], min: -0.6, max: 0.6, home: 0, continuous: false, channel: "steer" },
    { id: "spin_rl", type: "spin", parent: "chassis", target: "RL", axis: [0, 0, 1], min: -1000000, max: 1000000, home: 0, continuous: true, channel: "drive" },
    { id: "spin_rr", type: "spin", parent: "chassis", target: "RR", axis: [0, 0, 1], min: -1000000, max: 1000000, home: 0, continuous: true, channel: "drive" }
  ];

  const parts = [
    { name: "chassis", geometry: builder.boxGeometry(), material: 0 },
    { name: "cabin", geometry: builder.boxGeometry(), material: 1 },
    { name: "wheel", geometry: builder.cylinderGeometry(16), material: 2 }
  ];

  const nodes = [
    builder.meshNode({
      name: "chassis",
      mesh: 0,
      translation: [0, bodyY, 0],
      scale: [dim.length, dim.height, dim.width],
      extras: { aevum: { role: "chassis", id: "chassis" } }
    }),
    builder.meshNode({
      name: "cabin",
      mesh: 0,
      translation: [-dim.length * 0.10, cabinY, 0],
      scale: [dim.length * 0.45, cabinHeight, dim.width * 0.78],
      extras: { aevum: { role: "cabin", id: "cabin" } }
    })
  ];

  wheels.forEach(function (wheel) {
    nodes.push(builder.meshNode({
      name: wheel.node,
      mesh: 1,
      translation: wheel.pos,
      scale: [wheel.radius, wheel.radius, wheel.width],
      extras: { aevum: { role: "wheel", id: wheel.id } }
    }));
  });

  const aevum = {
    schema: meta.SCHEMA,
    kind: "vehicle",
    placeholder: true,
    units: "m",
    coordinate_system: meta.coordinateSystem(),
    source: { kind: "generated", note: "占位尺寸；不是实车测量值" },
    chassis: {
      length: dim.length,
      width: dim.width,
      height: dim.height,
      wheelbase: dim.wheelbase,
      track: dim.track,
      ground_clearance: dim.ground_clearance,
      mass: dim.mass
    },
    wheels: wheels,
    joints: joints,
    control: {
      loop_hz: 100,
      dt: 0.01,
      channels: { steer: ["steer_fl", "steer_fr"], drive: ["spin_rl", "spin_rr"] },
      limits: { steer: 0.6, speed: 1.0, timeout_ms: 200 }
    },
    sensors: { wheel_speed: false, steering: false }
  };

  const gltf = builder.buildModel({
    name: "TOY OFFROAD PLACEHOLDER",
    sceneName: "TOY OFFROAD PLACEHOLDER",
    parts: parts,
    nodes: nodes,
    materials: [
      builder.pbrMaterial("chassis", CHASSIS_COLOR, 0.8),
      builder.pbrMaterial("cabin", CABIN_COLOR, 0.7),
      builder.pbrMaterial("wheel", WHEEL_COLOR, 0.9)
    ],
    aevum: aevum
  });

  validate(gltf);
  return gltf;
}

function validate(gltf) {
  builder.validateGltf(gltf);
  meta.validate(gltf, "vehicle");

  const aevum = gltf.asset.extras.aevum;
  const chassis = aevum.chassis;
  if (!chassis) throw new Error("vehicle: 缺少 chassis");
  ["length", "width", "height", "wheelbase", "track", "mass"].forEach(function (key) {
    if (!(Number.isFinite(chassis[key]) && chassis[key] > 0)) throw new Error("vehicle: chassis." + key + " 必须是正数");
  });
  if (!(chassis.ground_clearance >= 0)) throw new Error("vehicle: ground_clearance 不能为负");

  if (!Array.isArray(aevum.wheels) || aevum.wheels.length < 3) throw new Error("vehicle: 至少需要 3 个轮子");
  const wheelIds = {};
  aevum.wheels.forEach(function (wheel) {
    if (!wheel.id || wheelIds[wheel.id]) throw new Error("vehicle: 轮子 id 重复或为空: " + wheel.id);
    wheelIds[wheel.id] = true;
    if (builder.nodeIndex(gltf, wheel.node) < 0) throw new Error("vehicle: 轮子 node 不存在: " + wheel.node);
    if (!(wheel.radius > 0 && wheel.width > 0)) throw new Error("vehicle: 轮子尺寸必须是正数: " + wheel.id);
  });

  aevum.joints.forEach(function (joint) {
    if (!wheelIds[joint.target]) throw new Error("vehicle: joint target 不是轮子: " + joint.id);
  });

  return true;
}

module.exports = {
  DIMENSIONS,
  build: build,
  validate: validate
};
