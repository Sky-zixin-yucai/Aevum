/*
 * 职责唯一：只负责玩具越野车的 glTF 模型语义与视觉件。
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

const COLORS = {
  body: [0.86, 0.34, 0.10],
  trim: [0.055, 0.060, 0.062],
  glass: [0.035, 0.095, 0.130],
  cream: [0.86, 0.81, 0.68],
  headlight: [1.00, 0.92, 0.62],
  taillight: [0.78, 0.10, 0.07],
  tire: [0.035, 0.035, 0.038],
  hub: [0.78, 0.80, 0.82]
};

function build() {
  const dim = Object.assign({}, DIMENSIONS);
  const halfTrack = dim.track / 2;
  const frontX = dim.wheelbase / 2;
  const rearX = -dim.wheelbase / 2;
  const wheelY = dim.wheel_radius;
  const chassisY = dim.ground_clearance + 0.030;
  const bodyY = 0.108;
  const cabinHeight = 0.042;
  const cabinY = 0.151;

  const wheels = [
    { id: "FL", node: "wheel_fl", tire: "tire_fl", front: true, pos: [frontX, wheelY, halfTrack], radius: dim.wheel_radius, width: dim.wheel_width },
    { id: "FR", node: "wheel_fr", tire: "tire_fr", front: true, pos: [frontX, wheelY, -halfTrack], radius: dim.wheel_radius, width: dim.wheel_width },
    { id: "RL", node: "wheel_rl", tire: "wheel_rl", front: false, pos: [rearX, wheelY, halfTrack], radius: dim.wheel_radius, width: dim.wheel_width },
    { id: "RR", node: "wheel_rr", tire: "wheel_rr", front: false, pos: [rearX, wheelY, -halfTrack], radius: dim.wheel_radius, width: dim.wheel_width }
  ];

  const joints = [
    { id: "steer_fl", type: "steer", parent: "chassis", target: "FL", axis: [0, 1, 0], min: -0.6, max: 0.6, home: 0, continuous: false, channel: "steer" },
    { id: "steer_fr", type: "steer", parent: "chassis", target: "FR", axis: [0, 1, 0], min: -0.6, max: 0.6, home: 0, continuous: false, channel: "steer" },
    { id: "spin_fl", type: "spin", parent: "FL", target: "FL.tire", axis: [0, 0, 1], min: -1000000, max: 1000000, home: 0, continuous: true, channel: "drive" },
    { id: "spin_fr", type: "spin", parent: "FR", target: "FR.tire", axis: [0, 0, 1], min: -1000000, max: 1000000, home: 0, continuous: true, channel: "drive" },
    { id: "spin_rl", type: "spin", parent: "RL", target: "RL", axis: [0, 0, 1], min: -1000000, max: 1000000, home: 0, continuous: true, channel: "drive" },
    { id: "spin_rr", type: "spin", parent: "RR", target: "RR", axis: [0, 0, 1], min: -1000000, max: 1000000, home: 0, continuous: true, channel: "drive" }
  ];

  const parts = [
    { name: "body", geometry: builder.boxGeometry(), material: 0 },
    { name: "trim", geometry: builder.boxGeometry(), material: 1 },
    { name: "glass", geometry: builder.boxGeometry(), material: 2 },
    { name: "cream", geometry: builder.boxGeometry(), material: 3 },
    { name: "headlight", geometry: builder.boxGeometry(), material: 4 },
    { name: "taillight", geometry: builder.boxGeometry(), material: 5 },
    { name: "wheel", geometry: builder.cylinderGeometry(18), material: 6 },
    { name: "hub", geometry: builder.cylinderGeometry(12), material: 7 }
  ];

  const nodes = [];
  function box(name, mesh, translation, scale, role, id, rotation) {
    nodes.push(builder.meshNode({
      name: name,
      mesh: mesh,
      translation: translation,
      scale: scale,
      rotation: rotation,
      extras: { aevum: { role: role, id: id } }
    }));
  }

  /* Running chassis and orange body shell. */
  box("chassis", 1, [0, chassisY, 0], [0.300, 0.060, 0.136], "chassis", "chassis");
  box("body", 0, [0, bodyY, 0], [0.310, 0.050, 0.170], "body", "body");
  box("hood", 0, [0.083, 0.137, 0], [0.132, 0.012, 0.176], "body", "hood");
  box("rocker_l", 1, [0, 0.078, 0.096], [0.250, 0.028, 0.012], "part", "rocker_l");
  box("rocker_r", 1, [0, 0.078, -0.096], [0.250, 0.028, 0.012], "part", "rocker_r");

  /* Cabin, glass, cream roof, and a small roof rack. */
  box("cabin", 0, [-0.032, cabinY, 0], [0.142, cabinHeight, 0.166], "cabin", "cabin");
  box("windshield", 2, [0.041, 0.151, 0], [0.006, 0.040, 0.134], "glass", "windshield", [0, 0, 0.314, 0.950]);
  box("rear_window", 2, [-0.105, 0.151, 0], [0.006, 0.036, 0.128], "glass", "rear_window", [0, 0, -0.279, 0.960]);
  box("side_glass_l", 2, [-0.034, 0.154, 0.084], [0.094, 0.024, 0.004], "glass", "side_glass_l");
  box("side_glass_r", 2, [-0.034, 0.154, -0.084], [0.094, 0.024, 0.004], "glass", "side_glass_r");
  box("roof", 3, [-0.032, 0.175, 0], [0.128, 0.008, 0.144], "roof", "roof");
  box("rack_l", 1, [-0.035, 0.181, 0.060], [0.118, 0.006, 0.010], "part", "rack_l");
  box("rack_r", 1, [-0.035, 0.181, -0.060], [0.118, 0.006, 0.010], "part", "rack_r");

  /* Bumpers, grille, lights, and side stripes. */
  box("bumper_f", 1, [0.168, 0.080, 0], [0.026, 0.034, 0.150], "part", "bumper_f");
  box("bumper_r", 1, [-0.168, 0.080, 0], [0.024, 0.034, 0.150], "part", "bumper_r");
  box("grille", 1, [0.160, 0.070, 0], [0.010, 0.026, 0.104], "part", "grille");
  box("headlight_l", 4, [0.160, 0.116, 0.063], [0.012, 0.016, 0.030], "light", "headlight_l");
  box("headlight_r", 4, [0.160, 0.116, -0.063], [0.012, 0.016, 0.030], "light", "headlight_r");
  box("taillight_l", 5, [-0.158, 0.116, 0.061], [0.010, 0.014, 0.026], "light", "taillight_l");
  box("taillight_r", 5, [-0.158, 0.116, -0.061], [0.010, 0.014, 0.026], "light", "taillight_r");
  box("stripe_l", 3, [0, 0.108, 0.086], [0.218, 0.010, 0.003], "part", "stripe_l");
  box("stripe_r", 3, [0, 0.108, -0.086], [0.218, 0.010, 0.003], "part", "stripe_r");

  /* Fender flares keep the tall tires visually attached to the body. */
  [[frontX, 1], [frontX, -1], [rearX, 1], [rearX, -1]].forEach(function (row, i) {
    box("fender_" + i, 0, [row[0], 0.081, row[1] * 0.102], [0.086, 0.016, 0.032], "part", "fender_" + i);
  });

  const childIndices = [];
  function addHub(parentIndex, wheel) {
    const hubName = "hub_" + wheel.id.toLowerCase();
    nodes.push(builder.meshNode({
      name: hubName,
      mesh: 7,
      translation: [0, 0, 0],
      scale: [0.58, 0.58, 1.65],
      extras: { aevum: { role: "part", id: hubName } }
    }));
    nodes[parentIndex].children = nodes[parentIndex].children || [];
    nodes[parentIndex].children.push(nodes.length - 1);
    childIndices.push(nodes.length - 1);
  }
  wheels.forEach(function (wheel) {
    if (wheel.front) {
      nodes.push(builder.meshNode({
        name: wheel.node,
        translation: wheel.pos,
        extras: { aevum: { role: "steer", id: wheel.id } }
      }));
      const pivotIndex = nodes.length - 1;
      nodes.push(builder.meshNode({
        name: wheel.tire,
        mesh: 6,
        scale: [wheel.radius, wheel.radius, wheel.width],
        extras: { aevum: { role: "wheel", id: wheel.id + ".tire" } }
      }));
      const tireIndex = nodes.length - 1;
      nodes[pivotIndex].children = [tireIndex];
      childIndices.push(tireIndex);
      addHub(tireIndex, wheel);
    } else {
      nodes.push(builder.meshNode({
        name: wheel.node,
        mesh: 6,
        translation: wheel.pos,
        scale: [wheel.radius, wheel.radius, wheel.width],
        extras: { aevum: { role: "wheel", id: wheel.id } }
      }));
      addHub(nodes.length - 1, wheel);
    }
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
      channels: { steer: ["steer_fl", "steer_fr"], drive: ["spin_fl", "spin_fr", "spin_rl", "spin_rr"] },
      limits: { steer: 0.6, speed: 1.0, timeout_ms: 200 }
    },
    sensors: { wheel_speed: false, steering: false }
  };

  const gltf = builder.buildModel({
    name: "TOY OFFROAD",
    sceneName: "TOY OFFROAD",
    parts: parts,
    nodes: nodes,
    materials: [
      builder.pbrMaterial("body_orange", COLORS.body, 0.55),
      builder.pbrMaterial("trim_black", COLORS.trim, 0.75),
      builder.pbrMaterial("glass_dark", COLORS.glass, 0.25),
      builder.pbrMaterial("roof_cream", COLORS.cream, 0.65),
      builder.pbrMaterial("headlight_warm", COLORS.headlight, 0.35),
      builder.pbrMaterial("taillight_red", COLORS.taillight, 0.45),
      builder.pbrMaterial("tire_black", COLORS.tire, 0.90),
      builder.pbrMaterial("hub_silver", COLORS.hub, 0.35)
    ],
    roots: nodes.map(function (_, i) { return i; }).filter(function (i) { return childIndices.indexOf(i) < 0; }),
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
    if (wheel.tire && builder.nodeIndex(gltf, wheel.tire) < 0) throw new Error("vehicle: 轮子 tire 节点不存在: " + wheel.tire);
    if (!(wheel.radius > 0 && wheel.width > 0)) throw new Error("vehicle: 轮子尺寸必须是正数: " + wheel.id);
  });

  const jointTargets = {};
  aevum.wheels.forEach(function (wheel) {
    jointTargets[wheel.id] = true;
    jointTargets[wheel.id + ".tire"] = true;
  });
  aevum.joints.forEach(function (joint) {
    if (!jointTargets[joint.target]) throw new Error("vehicle: joint target 不是轮子: " + joint.id);
  });

  return true;
}

module.exports = {
  DIMENSIONS,
  COLORS,
  build: build,
  validate: validate
};
