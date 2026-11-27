/* Minimal glTF 2.0 / GLB importer: nodes, meshes, materials; no skin/animation. */
(function (F) {
  "use strict";

  const COMPONENTS = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT2: 4, MAT3: 9, MAT4: 16 };
  const COMP_SIZE = { 5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4 };
  const GLB_MAGIC = 0x46546C67;
  const CHUNK_JSON = 0x4E4F534A;
  const CHUNK_BIN = 0x004E4942;

  function toBytes(data) {
    if (data instanceof Uint8Array) return data;
    if (data instanceof ArrayBuffer) return new Uint8Array(data);
    if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    throw new Error("unsupported buffer type");
  }

  function decodeText(bytes) {
    return new TextDecoder("utf-8").decode(bytes);
  }

  function parseGlb(data) {
    const bytes = toBytes(data);
    if (bytes.byteLength < 12) throw new Error("GLB too short");
    const magic = new DataView(bytes.buffer, bytes.byteOffset).getUint32(0, true);
    if (magic !== GLB_MAGIC) throw new Error("not a GLB (bad magic)");
    const version = new DataView(bytes.buffer, bytes.byteOffset).getUint32(4, true);
    if (version !== 2) throw new Error("GLB version " + version + " not supported");
    let p = 12;
    let json = null;
    let bin = null;
    while (p + 8 <= bytes.byteLength) {
      const len = new DataView(bytes.buffer, bytes.byteOffset).getUint32(p, true);
      const type = new DataView(bytes.buffer, bytes.byteOffset).getUint32(p + 4, true);
      const start = p + 8;
      const end = start + len;
      if (end > bytes.byteLength) throw new Error("GLB chunk overrun");
      if (type === CHUNK_JSON) json = JSON.parse(decodeText(bytes.subarray(start, end)).trim());
      else if (type === CHUNK_BIN) bin = bytes.subarray(start, end);
      p = end;
    }
    if (!json) throw new Error("GLB has no JSON chunk");
    return { json: json, bin: bin };
  }

  function parseDataUri(uri) {
    const comma = uri.indexOf(",");
    if (comma < 0) throw new Error("bad data URI");
    const meta = uri.slice(0, comma);
    const body = uri.slice(comma + 1);
    if (meta.indexOf(";base64") >= 0) {
      const raw = atob(body);
      const out = new Uint8Array(raw.length);
      for (let i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i);
      return out;
    }
    return new TextEncoder().encode(decodeURIComponent(body));
  }

  function findFile(files, uri) {
    const clean = decodeURIComponent(uri).replace(/\\/g, "/");
    const base = clean.split("/").pop().toLowerCase();
    for (let i = 0; i < files.length; i++) {
      const name = String(files[i].name || "").replace(/\\/g, "/").toLowerCase();
      if (name === clean.toLowerCase() || name.split("/").pop() === base) return files[i].data;
    }
    return null;
  }

  function loadBuffers(gltf, files, bin) {
    return (gltf.buffers || []).map(function (b, i) {
      if (b.uri) {
        if (String(b.uri).indexOf("data:") === 0) return parseDataUri(b.uri);
        const found = findFile(files || [], b.uri);
        if (!found) throw new Error("缺少外部 buffer: " + b.uri);
        return toBytes(found);
      }
      if (i === 0 && bin) return bin;
      throw new Error("buffer " + i + " 没有 uri，也没有 GLB BIN chunk");
    });
  }

  function accessorValues(gltf, index, buffers) {
    const a = (gltf.accessors || [])[index];
    if (!a) throw new Error("accessor " + index + " missing");
    const comps = COMPONENTS[a.type] || 1;
    const count = Number(a.count) || 0;
    const size = COMP_SIZE[a.componentType];
    if (!size) throw new Error("unsupported componentType " + a.componentType);
    if (a.bufferView == null) return { count: count, comps: comps, values: new Array(count * comps).fill(0) };
    const view = gltf.bufferViews[a.bufferView];
    const buf = buffers[view.buffer];
    if (!buf) throw new Error("bufferView " + a.bufferView + " has no buffer");
    const offset = (view.byteOffset || 0) + (a.byteOffset || 0);
    const stride = view.byteStride || comps * size;
    const dv = new DataView(buf.buffer, buf.byteOffset);
    const out = new Array(count * comps);
    let p = offset;
    for (let i = 0; i < count; i++) {
      for (let c = 0; c < comps; c++) {
        const q = p + c * size;
        let v;
        if (a.componentType === 5126) v = dv.getFloat32(q, true);
        else if (a.componentType === 5125) v = dv.getUint32(q, true);
        else if (a.componentType === 5123) v = dv.getUint16(q, true);
        else if (a.componentType === 5122) v = dv.getInt16(q, true);
        else if (a.componentType === 5121) v = dv.getUint8(q);
        else v = dv.getInt8(q);
        if (a.normalized && a.componentType !== 5126) {
          if (a.componentType === 5121) v /= 255;
          else if (a.componentType === 5123) v /= 65535;
          else if (a.componentType === 5120) v = Math.max(v / 127, -1);
          else if (a.componentType === 5122) v = Math.max(v / 32767, -1);
        }
        out[i * comps + c] = v;
      }
      p += stride;
    }
    return { count: count, comps: comps, values: out };
  }

  function materialColor(gltf, primitive) {
    const mat = (gltf.materials || [])[primitive.material];
    const f = mat && mat.pbrMetallicRoughness && mat.pbrMetallicRoughness.baseColorFactor;
    return Array.isArray(f) && f.length >= 3 ? [+f[0], +f[1], +f[2]] : [0.38, 0.58, 0.68];
  }

  function normalOf(a, b, c) {
    const ab = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
    const ac = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
    let n = [
      ab[1] * ac[2] - ab[2] * ac[1],
      ab[2] * ac[0] - ab[0] * ac[2],
      ab[0] * ac[1] - ab[1] * ac[0]
    ];
    const l = Math.hypot(n[0], n[1], n[2]) || 1;
    n = [n[0] / l, n[1] / l, n[2] / l];
    return n;
  }

  function readMesh(gltf, meshIndex, buffers) {
    const mesh = (gltf.meshes || [])[meshIndex];
    if (!mesh) return null;
    const positions = [];
    const normals = [];
    let color = null;
    let triangles = 0;
    (mesh.primitives || []).forEach(function (p) {
      if ((p.mode === undefined ? 4 : p.mode) !== 4) return;
      if (p.attributes == null || p.attributes.POSITION == null) return;
      const pos = accessorValues(gltf, p.attributes.POSITION, buffers);
      const nor = p.attributes.NORMAL != null ? accessorValues(gltf, p.attributes.NORMAL, buffers) : null;
      let idx;
      if (p.indices != null) idx = accessorValues(gltf, p.indices, buffers).values;
      else {
        idx = new Array(pos.count);
        for (let i = 0; i < pos.count; i++) idx[i] = i;
      }
      if (!color) color = materialColor(gltf, p);
      for (let i = 0; i + 2 < idx.length; i += 3) {
        const ia = idx[i], ib = idx[i + 1], ic = idx[i + 2];
        if (ia >= pos.count || ib >= pos.count || ic >= pos.count) continue;
        const a = [pos.values[ia * 3], pos.values[ia * 3 + 1], pos.values[ia * 3 + 2]];
        const b = [pos.values[ib * 3], pos.values[ib * 3 + 1], pos.values[ib * 3 + 2]];
        const c = [pos.values[ic * 3], pos.values[ic * 3 + 1], pos.values[ic * 3 + 2]];
        const fn = normalOf(a, b, c);
        [ia, ib, ic].forEach(function (vi, k) {
          const v = k === 0 ? a : (k === 1 ? b : c);
          positions.push(v[0], v[1], v[2]);
          if (nor && nor.count > vi) {
            normals.push(nor.values[vi * 3], nor.values[vi * 3 + 1], nor.values[vi * 3 + 2]);
          } else {
            normals.push(fn[0], fn[1], fn[2]);
          }
        });
        triangles++;
      }
    });
    if (!positions.length) return null;
    const min = [Infinity, Infinity, Infinity];
    const max = [-Infinity, -Infinity, -Infinity];
    for (let i = 0; i < positions.length; i += 3) {
      for (let c = 0; c < 3; c++) {
        min[c] = Math.min(min[c], positions[i + c]);
        max[c] = Math.max(max[c], positions[i + c]);
      }
    }
    return {
      positions: positions,
      normals: normals,
      color: color || [0.38, 0.58, 0.68],
      triangles: triangles,
      bounds: {
        min: min,
        max: max,
        size: [Math.max(max[0] - min[0], 0.001), Math.max(max[1] - min[1], 0.001), Math.max(max[2] - min[2], 0.001)]
      }
    };
  }

  function quatFromMat(m) {
    const m00 = m[0], m10 = m[1], m20 = m[2];
    const m01 = m[4], m11 = m[5], m21 = m[6];
    const m02 = m[8], m12 = m[9], m22 = m[10];
    const trace = m00 + m11 + m22;
    let x, y, z, w;
    if (trace > 0) {
      const s = Math.sqrt(trace + 1) * 2;
      w = 0.25 * s;
      x = (m21 - m12) / s;
      y = (m02 - m20) / s;
      z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
      const s = Math.sqrt(1 + m00 - m11 - m22) * 2;
      w = (m21 - m12) / s;
      x = 0.25 * s;
      y = (m01 + m10) / s;
      z = (m02 + m20) / s;
    } else if (m11 > m22) {
      const s = Math.sqrt(1 + m11 - m00 - m22) * 2;
      w = (m02 - m20) / s;
      x = (m01 + m10) / s;
      y = 0.25 * s;
      z = (m12 + m21) / s;
    } else {
      const s = Math.sqrt(1 + m22 - m00 - m11) * 2;
      w = (m10 - m01) / s;
      x = (m02 + m20) / s;
      y = (m12 + m21) / s;
      z = 0.25 * s;
    }
    const n = Math.hypot(x, y, z, w) || 1;
    return [x / n, y / n, z / n, w / n];
  }

  function decompose(m) {
    const t = [m[12], m[13], m[14]];
    let sx = Math.hypot(m[0], m[1], m[2]);
    let sy = Math.hypot(m[4], m[5], m[6]);
    let sz = Math.hypot(m[8], m[9], m[10]);
    if (sx < 1e-8) sx = 1;
    if (sy < 1e-8) sy = 1;
    if (sz < 1e-8) sz = 1;
    const n = new Float32Array(16);
    n[0] = m[0] / sx; n[1] = m[1] / sx; n[2] = m[2] / sx;
    n[4] = m[4] / sy; n[5] = m[5] / sy; n[6] = m[6] / sy;
    n[8] = m[8] / sz; n[9] = m[9] / sz; n[10] = m[10] / sz;
    n[15] = 1;
    let det = n[0] * (n[5] * n[10] - n[9] * n[6])
      - n[4] * (n[1] * n[10] - n[9] * n[2])
      + n[8] * (n[1] * n[6] - n[5] * n[2]);
    if (det < 0) sx = -sx;
    return { translation: t, rotation: quatFromMat(n), scale: [sx, sy, sz] };
  }

  function nodeLocal(node) {
    if (Array.isArray(node.matrix) && node.matrix.length === 16) {
      return decompose(node.matrix);
    }
    return {
      translation: Array.isArray(node.translation) ? node.translation.slice(0, 3) : [0, 0, 0],
      rotation: Array.isArray(node.rotation) ? node.rotation.slice(0, 4) : [0, 0, 0, 1],
      scale: Array.isArray(node.scale) ? node.scale.slice(0, 3) : [1, 1, 1]
    };
  }

  function inferAxis(node, local) {
    if (node.extras && Array.isArray(node.extras.jointAxis)) {
      const a = node.extras.jointAxis;
      return [Number(a[0]) || 0, Number(a[1]) || 0, Number(a[2]) || 1];
    }
    const t = local.translation;
    if (Math.abs(t[0]) >= Math.abs(t[1]) && Math.abs(t[0]) >= Math.abs(t[2])) return [0, 0, 1];
    if (Math.abs(t[1]) >= Math.abs(t[2])) return [0, 0, 1];
    return [0, 1, 0];
  }

  function sanitize(name, fallback) {
    const s = String(name || fallback).replace(/[^A-Za-z0-9_]+/g, "_").replace(/^_+|_+$/g, "");
    return s || fallback;
  }

  function buildDocument(gltf, buffers, name) {
    const nodes = gltf.nodes || [];
    const segments = [];
    const usedIds = {};
    const nodeSeg = {};

    function addNode(nodeIndex, parentSeg) {
      if (nodeSeg[nodeIndex] != null) return;
      const node = nodes[nodeIndex];
      if (!node) return;
      const local = nodeLocal(node);
      let id = sanitize(node.name, "L" + nodeIndex);
      if (usedIds[id]) {
        let n = 2;
        while (usedIds[id + "_" + n]) n++;
        id = id + "_" + n;
      }
      usedIds[id] = true;
      const meshData = node.mesh != null ? readMesh(gltf, node.mesh, buffers) : null;
      const nodeMeta = (node.extras && node.extras.aevum) || null;
      const seg = {
        id: id,
        name: node.name || id,
        parent: parentSeg,
        role: nodeMeta && nodeMeta.role || null,
        semanticId: nodeMeta && nodeMeta.id || null,
        actuated: false,
        q: 0,
        target: 0,
        min: -170,
        max: 170,
        torque: 5,
        inertia: 0.02,
        damping: 0.05,
        kp: 30,
        kd: 7,
        length: meshData ? meshData.bounds.size[0] : 0.2,
        width: meshData ? meshData.bounds.size[1] : 0.06,
        depth: meshData ? meshData.bounds.size[2] : 0.06,
        color: meshData ? meshData.color : [0.35, 0.62, 0.72],
        joint: {
          origin: local.translation,
          axis: inferAxis(node, local)
        },
        local: {
          translation: [0, 0, 0],
          rotation: local.rotation,
          scale: local.scale
        },
        mesh: meshData
      };
      nodeSeg[nodeIndex] = segments.length;
      segments.push(seg);
      (node.children || []).forEach(function (child) { addNode(child, nodeSeg[nodeIndex]); });
    }

    let roots = [];
    if (gltf.scenes && gltf.scenes.length) {
      const scene = gltf.scene != null ? gltf.scenes[gltf.scene] : gltf.scenes[0];
      if (scene && Array.isArray(scene.nodes)) roots = scene.nodes.slice();
    }
    if (!roots.length) {
      const hasParent = {};
      nodes.forEach(function (n) {
        (n.children || []).forEach(function (c) { hasParent[c] = true; });
      });
      nodes.forEach(function (_, i) { if (!hasParent[i]) roots.push(i); });
    }
    roots.forEach(function (r) { addNode(r, -1); });

    const aevum = gltf.asset && gltf.asset.extras && gltf.asset.extras.aevum || null;
    if (aevum && Array.isArray(aevum.joints) && aevum.joints.length) {
      const byTarget = {};
      aevum.joints.forEach(function (joint) {
        if (!joint || !joint.target) return;
        if (!byTarget[joint.target]) byTarget[joint.target] = [];
        byTarget[joint.target].push(joint);
      });
      segments.forEach(function (seg) {
        const joints = seg.semanticId ? byTarget[seg.semanticId] : null;
        seg.actuated = !!(joints && joints.length);
        if (!seg.actuated) return;
        const joint = joints[0];
        seg.jointType = joint.type || "";
        seg.channel = joint.channel || "";
        seg.jointCount = joints.length;
        seg.continuous = !!joint.continuous;
        seg.joint.axis = Array.isArray(joint.axis) ? joint.axis.slice(0, 3) : seg.joint.axis;
        if (Number.isFinite(joint.min)) seg.min = joint.min * 180 / Math.PI;
        if (Number.isFinite(joint.max)) seg.max = joint.max * 180 / Math.PI;
        if (Number.isFinite(joint.home)) {
          seg.q = joint.home;
          seg.target = joint.home;
        }
      });
    }

    const doc = {
      format: "glTF 2.0",
      name: name || "IMPORTED GLTF",
      modelKind: aevum && aevum.kind || "generic",
      aevum: aevum || null,
      base: { w: 0.01, h: 0.01, d: 0.01, mass: 0, color: [0.1, 0.1, 0.1], hidden: true },
      segments: segments,
      tool: null,
      control: aevum && aevum.control || { loop_hz: 100, dt: 0.01 },
      source: {
        kind: "gltf",
        format: "glTF 2.0",
        file: name || "",
        nodes: nodes.length,
        meshes: (gltf.meshes || []).length,
        scene: gltf.scene == null ? 0 : gltf.scene
      },
      meta: {
        imported: true,
        segmentCount: segments.length,
        meshCount: (gltf.meshes || []).length,
        triangleCount: segments.reduce(function (a, s) { return a + (s.mesh ? s.mesh.triangles : 0); }, 0)
      }
    };

    /*
     * glTF 节点可以是平铺的世界坐标外观件，但 aevum.body 已经声明了平面链。
     * 这里把语义关节按 body.links 的顺序接成控制链，并采用声明长度，
     * 不用 mesh 单位盒的包围盒长度误当关节长度。
     */
    if (aevum && aevum.kind === "arm" && aevum.body && Array.isArray(aevum.body.links)) {
      const body = aevum.body;
      const base = body.base || {};
      const tool = body.tool || {};
      const baseSeg = segments.find(function (s) { return s.semanticId === "base"; }) ||
        segments.find(function (s) { return s.role === "base"; });
      doc.base = {
        w: Number(base.width) || 0.54,
        h: Number(base.height) || 0.24,
        d: Number(base.depth) || 0.38,
        mass: Number(base.mass) || 2.2,
        color: baseSeg && baseSeg.color || [0.15, 0.19, 0.23],
        hidden: false
      };
      doc.tool = {
        length: Number(tool.length) || 0.20,
        width: Number(tool.width) || 0.075,
        depth: Number(tool.depth) || 0.09,
        mass: Number(tool.mass) || 0.1,
        grip: 0
      };

      const byId = {};
      segments.forEach(function (s) {
        if (s.semanticId) byId[s.semanticId] = s;
      });
      if (baseSeg) {
        baseSeg.parent = -1;
        baseSeg.length = doc.base.w;
        baseSeg.width = doc.base.h;
        baseSeg.depth = doc.base.d;
      }
      let prev = baseSeg ? segments.indexOf(baseSeg) : -1;
      body.links.forEach(function (link) {
        const seg = byId[link && link.id];
        if (!seg) return;
        seg.parent = prev;
        seg.length = Number(link.length) || seg.length;
        seg.width = Number(link.width) || seg.width;
        seg.depth = Number(link.depth) || seg.depth;
        seg.mass = Number(link.mass) || seg.mass;
        prev = segments.indexOf(seg);
      });
      const toolSeg = segments.find(function (s) { return s.semanticId === "tool"; }) ||
        segments.find(function (s) { return s.role === "tool"; });
      if (toolSeg) {
        toolSeg.parent = prev;
        toolSeg.length = doc.tool.length;
        toolSeg.width = doc.tool.width;
        toolSeg.depth = doc.tool.depth;
      }
    }

    return doc;
  }

  function importGltfFiles(files, mainName) {
    const list = files || [];
    let main = null;
    if (mainName) main = list.find(function (f) { return f.name === mainName; }) || null;
    if (!main) main = list.find(function (f) { return /\.(glb|gltf)$/i.test(f.name || ""); }) || null;
    if (!main) throw new Error("没有 .glb/.gltf 文件");
    const bytes = toBytes(main.data);
    const baseName = String(main.name || "model.glb");
    let gltf, bin = null;
    if (/\.glb$/i.test(baseName)) {
      const parsed = parseGlb(bytes);
      gltf = parsed.json;
      bin = parsed.bin;
    } else {
      gltf = JSON.parse(decodeText(bytes));
    }
    const buffers = loadBuffers(gltf, list, bin);
    return F.model.validate(buildDocument(gltf, buffers, baseName));
  }

  function importGltfBuffer(buffer, name) {
    return importGltfFiles([{ name: name || "model.glb", data: buffer }], name || "model.glb");
  }

  F.model.importGltfFiles = importGltfFiles;
  F.model.importGltfBuffer = importGltfBuffer;
  F.model.buildImportedDocument = buildDocument;
})(window.AEVUM.FORGE);
