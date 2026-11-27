/*
 * 职责唯一：只负责 glTF 2.0 结构组装。
 *
 * 不认车辆、不认机械臂、不认 Aevum 语义。
 * 几何 primitive、buffer、accessor、mesh、node、material、scene 都在这里。
 */
"use strict";

const FORMAT = "glTF 2.0";

function boxGeometry() {
  const positions = [];
  const normals = [];
  const indices = [];
  const faces = [
    { n: [1, 0, 0], v: [[.5, -.5, -.5], [.5, .5, -.5], [.5, .5, .5], [.5, -.5, .5]] },
    { n: [-1, 0, 0], v: [[-.5, -.5, .5], [-.5, .5, .5], [-.5, .5, -.5], [-.5, -.5, -.5]] },
    { n: [0, 1, 0], v: [[-.5, .5, -.5], [-.5, .5, .5], [.5, .5, .5], [.5, .5, -.5]] },
    { n: [0, -1, 0], v: [[-.5, -.5, .5], [-.5, -.5, -.5], [.5, -.5, -.5], [.5, -.5, .5]] },
    { n: [0, 0, 1], v: [[-.5, -.5, .5], [.5, -.5, .5], [.5, .5, .5], [-.5, .5, .5]] },
    { n: [0, 0, -1], v: [[.5, -.5, -.5], [-.5, -.5, -.5], [-.5, .5, -.5], [.5, .5, -.5]] }
  ];

  faces.forEach(function (face, fi) {
    const base = fi * 4;
    face.v.forEach(function (v) {
      positions.push(v[0], v[1], v[2]);
      normals.push(face.n[0], face.n[1], face.n[2]);
    });
    indices.push(base, base + 1, base + 2, base, base + 2, base + 3);
  });

  return { positions, normals, indices };
}

function cylinderGeometry(segments) {
  const positions = [];
  const normals = [];
  const indices = [];
  const n = segments || 16;
  let i;

  for (i = 0; i <= n; i++) {
    const a = (i / n) * Math.PI * 2;
    const cx = Math.cos(a);
    const sy = Math.sin(a);
    positions.push(cx, sy, -0.5);
    normals.push(cx, sy, 0);
    positions.push(cx, sy, 0.5);
    normals.push(cx, sy, 0);
  }
  for (i = 0; i < n; i++) {
    const p = i * 2;
    indices.push(p, p + 1, p + 2, p + 1, p + 3, p + 2);
  }

  const bottomCenter = positions.length / 3;
  positions.push(0, 0, -0.5);
  normals.push(0, 0, -1);
  const bottomStart = positions.length / 3;
  for (i = 0; i <= n; i++) {
    const a = (i / n) * Math.PI * 2;
    positions.push(Math.cos(a), Math.sin(a), -0.5);
    normals.push(0, 0, -1);
  }
  for (i = 0; i < n; i++) {
    indices.push(bottomCenter, bottomStart + i, bottomStart + i + 1);
  }

  const topCenter = positions.length / 3;
  positions.push(0, 0, 0.5);
  normals.push(0, 0, 1);
  const topStart = positions.length / 3;
  for (i = 0; i <= n; i++) {
    const a = (i / n) * Math.PI * 2;
    positions.push(Math.cos(a), Math.sin(a), 0.5);
    normals.push(0, 0, 1);
  }
  for (i = 0; i < n; i++) {
    indices.push(topCenter, topStart + i + 1, topStart + i);
  }

  return { positions, normals, indices };
}

function accessorMinMax(values) {
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < values.length; i += 3) {
    for (let c = 0; c < 3; c++) {
      min[c] = Math.min(min[c], values[i + c]);
      max[c] = Math.max(max[c], values[i + c]);
    }
  }
  return { min, max };
}

function pbrMaterial(name, color, roughness) {
  if (!Array.isArray(color) || color.length < 3 || !color.slice(0, 3).every(Number.isFinite)) {
    throw new Error("pbrMaterial: color 至少需要 3 个数值");
  }
  const factor = color.length === 4 ? color.slice(0, 4) : [color[0], color[1], color[2], 1];
  return {
    name: name,
    pbrMetallicRoughness: {
      baseColorFactor: factor,
      metallicFactor: 0,
      roughnessFactor: Number.isFinite(roughness) ? roughness : 1
    }
  };
}

function meshNode(spec) {
  const node = { name: spec.name };
  if (spec.mesh != null) node.mesh = spec.mesh;
  if (spec.translation) node.translation = spec.translation;
  if (spec.rotation) node.rotation = spec.rotation;
  if (spec.scale) node.scale = spec.scale;
  if (spec.extras) node.extras = spec.extras;
  return node;
}

function nodeIndex(gltf, name) {
  if (!Array.isArray(gltf.nodes)) return -1;
  return gltf.nodes.findIndex(function (node) { return node.name === name; });
}

function buildBuffer(parts) {
  const chunks = [];
  let offset = 0;

  function align4() {
    while (offset % 4 !== 0) {
      chunks.push(Buffer.from([0]));
      offset++;
    }
  }

  function addFloat32(values) {
    align4();
    const data = Buffer.alloc(values.length * 4);
    for (let i = 0; i < values.length; i++) data.writeFloatLE(values[i], i * 4);
    chunks.push(data);
    const start = offset;
    offset += data.length;
    return { byteOffset: start, byteLength: data.length };
  }

  function addUint16(values) {
    align4();
    const data = Buffer.alloc(values.length * 2);
    for (let i = 0; i < values.length; i++) data.writeUInt16LE(values[i], i * 2);
    chunks.push(data);
    const start = offset;
    offset += data.length;
    return { byteOffset: start, byteLength: data.length };
  }

  const views = [];
  const accessors = [];

  function addVector(values) {
    const v = addFloat32(values);
    views.push({ buffer: 0, byteOffset: v.byteOffset, byteLength: v.byteLength, target: 34962 });
    const viewIndex = views.length - 1;
    const mm = accessorMinMax(values);
    accessors.push({
      bufferView: viewIndex,
      componentType: 5126,
      count: values.length / 3,
      type: "VEC3",
      min: mm.min,
      max: mm.max
    });
    return accessors.length - 1;
  }

  function addIndices(values) {
    const v = addUint16(values);
    views.push({ buffer: 0, byteOffset: v.byteOffset, byteLength: v.byteLength, target: 34963 });
    const viewIndex = views.length - 1;
    accessors.push({ bufferView: viewIndex, componentType: 5123, count: values.length, type: "SCALAR" });
    return accessors.length - 1;
  }

  const meshes = [];
  parts.forEach(function (part) {
    const pos = addVector(part.geometry.positions);
    const norm = addVector(part.geometry.normals);
    const idx = addIndices(part.geometry.indices);
    meshes.push({
      name: part.name,
      primitives: [{
        attributes: { POSITION: pos, NORMAL: norm },
        indices: idx,
        material: part.material
      }]
    });
  });

  const data = Buffer.concat(chunks);
  return {
    meshes,
    buffer: {
      byteLength: data.length,
      uri: "data:application/octet-stream;base64," + data.toString("base64")
    },
    bufferViews: views,
    accessors
  };
}

function buildModel(input) {
  if (!input || !input.name) throw new Error("buildModel: name required");
  if (!Array.isArray(input.parts) || !input.parts.length) throw new Error("buildModel: parts required");
  if (!Array.isArray(input.nodes) || !input.nodes.length) throw new Error("buildModel: nodes required");
  if (!Array.isArray(input.materials) || !input.materials.length) throw new Error("buildModel: materials required");
  if (!input.aevum || typeof input.aevum !== "object") throw new Error("buildModel: aevum metadata required");

  const built = buildBuffer(input.parts);
  const gltf = {
    asset: {
      version: "2.0",
      generator: input.generator || "Aevum glTF model generator",
      extras: { aevum: input.aevum }
    },
    scene: 0,
    scenes: [{
      name: input.sceneName || input.name,
      nodes: Array.isArray(input.roots)
        ? input.roots.slice()
        : input.nodes.map(function (_, index) { return index; })
    }],
    nodes: input.nodes,
    meshes: built.meshes,
    materials: input.materials,
    buffers: [built.buffer],
    bufferViews: built.bufferViews,
    accessors: built.accessors
  };

  validateGltf(gltf);
  return gltf;
}

function validateGltf(gltf) {
  if (!gltf || gltf.asset?.version !== "2.0") throw new Error("不是 glTF 2.0");
  if (!Array.isArray(gltf.nodes) || !gltf.nodes.length) throw new Error("缺少 nodes");
  if (!Array.isArray(gltf.meshes) || !gltf.meshes.length) throw new Error("缺少 meshes");
  if (!Array.isArray(gltf.materials) || !gltf.materials.length) throw new Error("缺少 materials");
  if (!Array.isArray(gltf.buffers) || !gltf.buffers.length) throw new Error("缺少 buffers");
  if (!gltf.scenes || !Array.isArray(gltf.scenes) || !gltf.scenes.length) throw new Error("缺少 scenes");
  if (gltf.scene == null || !gltf.scenes[gltf.scene]) throw new Error("scene 索引无效");

  gltf.nodes.forEach(function (node, index) {
    if (node.mesh != null && !gltf.meshes[node.mesh]) throw new Error("node " + index + " mesh 无效");
  });
  gltf.meshes.forEach(function (mesh, index) {
    if (!mesh.primitives || !mesh.primitives.length) throw new Error("mesh " + index + " 没有 primitive");
  });
  gltf.bufferViews.forEach(function (view, index) {
    if (!(view.byteOffset >= 0 && view.byteLength > 0)) throw new Error("bufferView " + index + " 无效");
  });
  return true;
}

function parse(text) {
  const gltf = JSON.parse(text);
  validateGltf(gltf);
  return gltf;
}

function stringify(gltf) {
  validateGltf(gltf);
  return JSON.stringify(gltf, null, 2) + "\n";
}

module.exports = {
  FORMAT,
  boxGeometry,
  cylinderGeometry,
  pbrMaterial,
  meshNode,
  nodeIndex,
  buildModel,
  validateGltf,
  parse,
  stringify
};
