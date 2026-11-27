/*
 * 职责唯一：只负责默认 3R 机械臂的 glTF 模型语义。
 *
 * glTF 结构由 gltf-builder.js 负责，公共 extras 语义由 model-meta.js 负责。
 * 本文件不写 buffer/accessor，也不处理车辆。
 */
"use strict";

const builder = require("../gltf/gltf-builder.js");
const meta = require("../gltf/model-meta.js");

const BASE = { width: 0.54, height: 0.24, depth: 0.38, mass: 2.2, color: [0.15, 0.19, 0.23] };
const LINKS = [
  { id: "J1", name: "SHOULDER", node: "link1", length: 0.62, width: 0.12, depth: 0.14, mass: 1.1, q: 1.05, minDeg: -170, maxDeg: 170, dynamics: { torque: 12, inertia: 0.035, damping: 0.05, kp: 38, kd: 9 }, color: [0.18, 0.62, 0.72] },
  { id: "J2", name: "ELBOW", node: "link2", length: 0.52, width: 0.10, depth: 0.12, mass: 0.8, q: -1.60, minDeg: -150, maxDeg: 150, dynamics: { torque: 9, inertia: 0.022, damping: 0.04, kp: 32, kd: 7 }, color: [0.22, 0.50, 0.70] },
  { id: "J3", name: "WRIST", node: "link3", length: 0.24, width: 0.075, depth: 0.09, mass: 0.35, q: 0.55, minDeg: -170, maxDeg: 170, dynamics: { torque: 5, inertia: 0.012, damping: 0.03, kp: 24, kd: 5 }, color: [0.70, 0.45, 0.18] }
];
const TOOL = { node: "tool", length: 0.20, width: 0.075, depth: 0.09, mass: 0.1, color: [0.82, 0.55, 0.22] };

function quatZ(angle) {
  return [0, 0, Math.sin(angle / 2), Math.cos(angle / 2)];
}

function linkTransform(start, angle, length) {
  const center = [
    start[0] + Math.cos(angle) * length / 2,
    start[1] + Math.sin(angle) * length / 2,
    0
  ];
  const end = [
    start[0] + Math.cos(angle) * length,
    start[1] + Math.sin(angle) * length,
    0
  ];
  return { center, end, angle };
}

function build() {
  const parts = [];
  const nodes = [];
  const materials = [];
  const links = [];
  const joints = [];

  let angle = 0;
  let start = [0, BASE.height, 0];

  parts.push({ name: "base", geometry: builder.boxGeometry(), material: 0 });
  materials.push(builder.pbrMaterial("base", BASE.color, 0.8));
  nodes.push(builder.meshNode({
    name: "base",
    mesh: 0,
    translation: [0, BASE.height / 2, 0],
    scale: [BASE.width, BASE.height, BASE.depth],
    extras: { aevum: { role: "base", id: "base" } }
  }));

  LINKS.forEach(function (link, index) {
    angle += link.q;
    const t = linkTransform(start, angle, link.length);
    const material = index + 1;
    parts.push({ name: link.node, geometry: builder.boxGeometry(), material: material });
    materials.push(builder.pbrMaterial(link.name.toLowerCase(), link.color, 0.7));
    nodes.push(builder.meshNode({
      name: link.node,
      mesh: material,
      translation: t.center,
      rotation: quatZ(t.angle),
      scale: [link.length, link.width, link.depth],
      extras: { aevum: { role: "link", id: link.id } }
    }));
    links.push({
      id: link.id,
      name: link.name,
      node: link.node,
      length: link.length,
      width: link.width,
      depth: link.depth,
      mass: link.mass,
      dynamics: link.dynamics
    });
    joints.push({
      id: link.id,
      type: "revolute",
      parent: index === 0 ? "base" : LINKS[index - 1].id,
      target: link.id,
      axis: [0, 0, 1],
      min: link.minDeg * Math.PI / 180,
      max: link.maxDeg * Math.PI / 180,
      home: link.q,
      continuous: false,
      channel: "joint"
    });
    start = t.end;
  });

  const toolMaterial = LINKS.length + 1;
  const toolCenter = [
    start[0] + Math.cos(angle) * TOOL.length / 2,
    start[1] + Math.sin(angle) * TOOL.length / 2,
    0
  ];
  parts.push({ name: "tool", geometry: builder.boxGeometry(), material: toolMaterial });
  materials.push(builder.pbrMaterial("tool", TOOL.color, 0.6));
  nodes.push(builder.meshNode({
    name: "tool",
    mesh: toolMaterial,
    translation: toolCenter,
    rotation: quatZ(angle),
    scale: [TOOL.length, TOOL.width, TOOL.depth],
    extras: { aevum: { role: "tool", id: "tool" } }
  }));

  const body = {
    base: {
      node: "base",
      width: BASE.width,
      height: BASE.height,
      depth: BASE.depth,
      mass: BASE.mass
    },
    links: links,
    tool: {
      node: TOOL.node,
      length: TOOL.length,
      width: TOOL.width,
      depth: TOOL.depth,
      mass: TOOL.mass
    }
  };

  const aevum = {
    schema: meta.SCHEMA,
    kind: "arm",
    placeholder: true,
    units: "m",
    coordinate_system: meta.coordinateSystem(),
    source: { kind: "generated-default", note: "Forge 3R 默认几何；不是实测尺寸" },
    body: body,
    joints: joints,
    control: {
      loop_hz: 100,
      dt: 0.01,
      channels: { joint: ["J1", "J2", "J3"] },
      limits: { speed: 3.0, timeout_ms: 200 }
    },
    sensors: {}
  };

  const gltf = builder.buildModel({
    name: "AEVUM 3R ARM PLACEHOLDER",
    sceneName: "AEVUM 3R ARM PLACEHOLDER",
    parts: parts,
    nodes: nodes,
    materials: materials,
    aevum: aevum
  });

  validate(gltf);
  return gltf;
}

function validate(gltf) {
  builder.validateGltf(gltf);
  meta.validate(gltf, "arm");

  const aevum = gltf.asset.extras.aevum;
  const body = aevum.body;
  if (!body || !body.base || !Array.isArray(body.links) || !body.tool) throw new Error("arm: 缺少 body.base/links/tool");
  ["width", "height", "depth", "mass"].forEach(function (key) {
    if (!(body.base[key] > 0)) throw new Error("arm: base." + key + " 必须是正数");
  });
  if (body.links.length !== aevum.joints.length) throw new Error("arm: links 和 joints 数量不一致");

  const linkIds = {};
  body.links.forEach(function (link) {
    if (!link.id || linkIds[link.id]) throw new Error("arm: link id 重复或为空: " + link.id);
    linkIds[link.id] = true;
    if (builder.nodeIndex(gltf, link.node) < 0) throw new Error("arm: link node 不存在: " + link.node);
    ["length", "width", "depth", "mass"].forEach(function (key) {
      if (!(link[key] > 0)) throw new Error("arm: link." + key + " 必须是正数: " + link.id);
    });
    if (!link.dynamics || typeof link.dynamics !== "object") throw new Error("arm: link.dynamics 缺失: " + link.id);
    ["torque", "inertia", "kp", "kd"].forEach(function (key) {
      if (!(link.dynamics[key] > 0)) throw new Error("arm: link.dynamics." + key + " 必须是正数: " + link.id);
    });
    if (!(link.dynamics.damping >= 0)) throw new Error("arm: link.dynamics.damping 不能为负: " + link.id);
  });
  if (builder.nodeIndex(gltf, body.tool.node) < 0) throw new Error("arm: tool node 不存在: " + body.tool.node);

  aevum.joints.forEach(function (joint) {
    if (!linkIds[joint.target]) throw new Error("arm: joint target 不是 link: " + joint.id);
  });

  return true;
}

module.exports = {
  build: build,
  validate: validate
};
