/* BodyDocument: schema/defaults for the native 3R and imported kinematic tree. */
(function (F) {
  "use strict";

  function defaultDoc() {
    return {
      format: "aevum.body/0",
      name: "AEVUM 3R ARM",
      base: { w: 0.54, h: 0.24, d: 0.38, mass: 2.2, color: [0.15, 0.19, 0.23] },
      segments: [
        { id: "J1", name: "SHOULDER", actuated: true, jointType: "revolute", q: 1.05, target: 1.05, min: -170, max: 170, torque: 12, inertia: 0.035, damping: 0.05, kp: 38, kd: 9, length: 0.62, width: 0.12, depth: 0.14, mass: 1.1, color: [0.18, 0.62, 0.72] },
        { id: "J2", name: "ELBOW", actuated: true, jointType: "revolute", q: -1.60, target: -1.60, min: -150, max: 150, torque: 9, inertia: 0.022, damping: 0.04, kp: 32, kd: 7, length: 0.52, width: 0.10, depth: 0.12, mass: 0.8, color: [0.22, 0.50, 0.70] },
        { id: "J3", name: "WRIST", actuated: true, jointType: "revolute", q: 0.55, target: 0.55, min: -170, max: 170, torque: 5, inertia: 0.012, damping: 0.03, kp: 24, kd: 5, length: 0.24, width: 0.075, depth: 0.09, mass: 0.35, color: [0.70, 0.45, 0.18] }
      ],
      tool: { length: 0.20, width: 0.075, depth: 0.09, grip: 0, color: [0.82, 0.55, 0.22] },
      prop: { type: "box", size: [0.12, 0.12, 0.12], pos: [1.30, 0.06, 0], color: [0.78, 0.42, 0.18] },
      control: { loop_hz: 100, dt: 0.01 }
    };
  }

  function clone(doc) {
    return JSON.parse(JSON.stringify(doc));
  }

  function norm3(v, fallback) {
    return Array.isArray(v) && v.length >= 3 ? [+v[0], +v[1], +v[2]] : fallback.slice();
  }

  function norm4(v) {
    if (!Array.isArray(v) || v.length < 4) return [0, 0, 0, 1];
    const q = [+v[0], +v[1], +v[2], +v[3]];
    const n = Math.hypot(q[0], q[1], q[2], q[3]) || 1;
    return [q[0] / n, q[1] / n, q[2] / n, q[3] / n];
  }

  function normalizeSegment(seg, index) {
    seg.id = seg.id || ("L" + index);
    seg.name = seg.name || seg.id;
    seg.parent = Number.isInteger(seg.parent) ? seg.parent : (index === 0 ? -1 : index - 1);
    seg.q = Number(seg.q) || 0;
    seg.target = Number.isFinite(seg.target) ? seg.target : seg.q;
    seg.min = Number.isFinite(seg.min) ? seg.min : -170;
    seg.max = Number.isFinite(seg.max) ? seg.max : 170;
    seg.torque = Number.isFinite(seg.torque) ? seg.torque : 5;
    seg.inertia = Number.isFinite(seg.inertia) ? Math.max(0.001, seg.inertia) : 0.02;
    seg.damping = Number.isFinite(seg.damping) ? seg.damping : 0.05;
    seg.kp = Number.isFinite(seg.kp) ? seg.kp : 30;
    seg.kd = Number.isFinite(seg.kd) ? seg.kd : 7;
    seg.length = Number.isFinite(seg.length) ? seg.length : 0.2;
    seg.width = Number.isFinite(seg.width) ? seg.width : 0.06;
    seg.depth = Number.isFinite(seg.depth) ? seg.depth : 0.06;
    seg.color = Array.isArray(seg.color) ? seg.color.slice(0, 3) : [0.35, 0.62, 0.72];
    if (!seg.local) seg.local = {};
    seg.local.translation = norm3(seg.local.translation, [0, 0, 0]);
    seg.local.rotation = norm4(seg.local.rotation);
    seg.local.scale = norm3(seg.local.scale, [1, 1, 1]);
    if (seg.joint) {
      seg.joint.origin = norm3(seg.joint.origin, [0, 0, 0]);
      seg.joint.axis = norm3(seg.joint.axis, [0, 0, 1]);
    }
    if (seg.actuated === undefined) seg.actuated = true;
    return seg;
  }

  function validate(doc) {
    if (!doc || !Array.isArray(doc.segments) || !doc.segments.length) {
      throw new Error("missing segments");
    }
    if (isImported(doc)) {
      if (!doc.control) doc.control = { loop_hz: 100, dt: 0.01 };
      if (!doc.base) {
        doc.base = { w: 0.01, h: 0.01, d: 0.01, mass: 0, color: [0.1, 0.1, 0.1], hidden: true };
      }
      doc.base.hidden = true;
      if (!("tool" in doc)) doc.tool = null;
      doc.segments.forEach(normalizeSegment);
      return doc;
    }
    if (!doc.base || !doc.tool || !doc.control) {
      throw new Error("missing base/tool/control");
    }
    doc.segments.forEach(normalizeSegment);
    return doc;
  }

  function isImported(doc) {
    return !!doc && (doc.format === "aevum.body/1" || !!(doc.source && doc.source.kind === "gltf"));
  }

  function segmentLength(seg) {
    if (seg.mesh && seg.mesh.bounds && seg.mesh.bounds.size) {
      return Math.max(seg.mesh.bounds.size[0], 0.02);
    }
    return Math.max(seg.length || 0.02, 0.02);
  }

  /*
   * 第一版内核只接受平面串联链：2–8 个关节、单链 parent、Z 轴、总 reach ≤ 16 m。
   * 几何不再要求固定 0.30/0.30/0.20；body descriptor 由页面在连接/换文档时下发。
   */
  function kernelCompatibility(doc) {
    const segs = doc && Array.isArray(doc.segments) ? doc.segments : [];
    if (segs.length < 2 || segs.length > 8) {
      return { ok: false, reason: "需要 2–8 个 segment，当前 " + segs.length };
    }
    for (let i = 0; i < segs.length; i++) {
      const s = segs[i];
      const parent = Number.isInteger(s.parent) ? s.parent : (i === 0 ? -1 : i - 1);
      if (i === 0 ? parent !== -1 : parent !== i - 1) {
        return { ok: false, reason: "需要单链 parent -1/0/1…，segment " + s.id + " 是 " + parent };
      }
      if (isImported(doc)) {
        if (!s.joint) return { ok: false, reason: "segment " + s.id + " 没有 joint" };
        const axis = s.joint.axis || [0, 0, 1];
        if (Math.abs(axis[2]) < 0.95) return { ok: false, reason: "segment " + s.id + " 的轴不是 Z" };
      }
      const len = segmentLength(s);
      if (!(len > 0 && len <= 2.0)) {
        return { ok: false, reason: "segment " + s.id + " 长度 " + len.toFixed(2) + " m 超出 0–2 m" };
      }
    }
    const reach = segs.reduce(function (sum, s) { return sum + segmentLength(s); }, 0);
    if (reach > 16.0) return { ok: false, reason: "总 reach " + reach.toFixed(2) + " m 超过 16 m" };
    return { ok: true, reason: segs.length + "R planar chain · " + reach.toFixed(2) + " m reach" };
  }

  function bodyDescriptor(doc) {
    const compat = kernelCompatibility(doc);
    if (!compat.ok) return null;
    const segs = doc.segments;
    const rad = function (v) { return (Number(v) || 0) * Math.PI / 180; };
    const clamp = function (v, lo, hi) { return Math.max(lo, Math.min(hi, v)); };
    return {
      n: segs.length,
      lengths: segs.map(function (s) { return +segmentLength(s).toFixed(6); }),
      qMin: segs.map(function (s) { return +clamp(rad(s.min), -3.2, 3.2).toFixed(6); }),
      qMax: segs.map(function (s) { return +clamp(rad(s.max), -3.2, 3.2).toFixed(6); }),
      qHome: segs.map(function (s) {
        const lo = clamp(rad(s.min), -3.2, 3.2);
        const hi = clamp(rad(s.max), -3.2, 3.2);
        return +clamp(Number(s.q) || 0, lo, hi).toFixed(6);
      })
    };
  }

  F.model = F.model || {};
  F.model.defaultDoc = defaultDoc;
  F.model.clone = clone;
  F.model.validate = validate;
  F.model.isImported = isImported;
  F.model.segmentLength = segmentLength;
  F.model.kernelCompatibility = kernelCompatibility;
  F.model.bodyDescriptor = bodyDescriptor;
})(window.AEVUM.FORGE);
