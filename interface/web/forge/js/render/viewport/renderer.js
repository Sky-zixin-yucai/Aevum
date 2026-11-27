/* Viewport renderer: owns the draw order, not the camera or the model. */
(function (F) {
  "use strict";

  const M = F.math;

  function normalize(v) {
    const l = Math.hypot(v[0], v[1], v[2]) || 1;
    return [v[0] / l, v[1] / l, v[2] / l];
  }

  function alignZ(axis) {
    const a = normalize(axis || [0, 0, 1]);
    if (a[2] > 0.9999) return M.mat4();
    if (a[2] < -0.9999) return M.axisAngle([1, 0, 0], Math.PI);
    const cross = [-a[1], a[0], 0];
    return M.axisAngle(cross, Math.acos(Math.max(-1, Math.min(1, a[2]))));
  }

  function axisBasis(axis) {
    const a = normalize(axis || [0, 0, 1]);
    let ref = Math.abs(a[0]) > 0.85 ? [0, 1, 0] : [1, 0, 0];
    let u = [
      a[1] * ref[2] - a[2] * ref[1],
      a[2] * ref[0] - a[0] * ref[2],
      a[0] * ref[1] - a[1] * ref[0]
    ];
    u = normalize(u);
    const v = [
      a[1] * u[2] - a[2] * u[1],
      a[2] * u[0] - a[0] * u[2],
      a[0] * u[1] - a[1] * u[0]
    ];
    return { u: u, v: v };
  }

  function axisColor(axis) {
    const a = normalize(axis || [0, 0, 1]);
    const ax = Math.abs(a[0]), ay = Math.abs(a[1]), az = Math.abs(a[2]);
    if (ax >= ay && ax >= az) return [0.88, 0.42, 0.46];
    if (ay >= ax && ay >= az) return [0.48, 0.85, 0.56];
    return [0.43, 0.66, 1.0];
  }

  const CLAY = [
    [0.62, 0.65, 0.67],
    [0.48, 0.54, 0.58],
    [0.70, 0.53, 0.36],
    [0.55, 0.59, 0.62]
  ];

  function mute(c) {
    return [
      0.36 + 0.58 * (c ? c[0] : 0.5),
      0.39 + 0.55 * (c ? c[1] : 0.5),
      0.42 + 0.52 * (c ? c[2] : 0.5)
    ];
  }

  function createRenderer(canvas, labels) {
    const ctx = F.render.createContext(canvas);
    const buffers = F.render.createBuffers(ctx);
    const gl = ctx.gl;
    const options = { grid: true, clay: false };

    function resize() {
      const dpr = Math.min(window.devicePixelRatio || 1, 1.5);
      const w = canvas.clientWidth || 1;
      const h = canvas.clientHeight || 1;
      if (canvas.width !== Math.floor(w * dpr) || canvas.height !== Math.floor(h * dpr)) {
        canvas.width = Math.floor(w * dpr);
        canvas.height = Math.floor(h * dpr);
      }
      gl.viewport(0, 0, canvas.width, canvas.height);
    }

    function useMesh(vp) {
      gl.useProgram(ctx.meshProg);
      gl.uniformMatrix4fv(ctx.meshLoc.uVP, false, vp);
    }

    function drawBuffer(buffer, model, color) {
      if (!buffer) return;
      buffers.drawMeshOpts(buffer, model, M.mat3(model), color, 1, 0);
    }

    function drawData(dataMesh, model, color) {
      const buffer = buffers.meshFor(dataMesh);
      drawBuffer(buffer, model, color);
    }

    function linkColor(seg, i, selected, doc) {
      if (selected) return [0.82, 0.86, 0.90];
      if (options.clay) return CLAY[i % CLAY.length];
      if (doc && doc.modelKind === "vehicle") {
        const v = seg.color || [0.6, 0.6, 0.6];
        return [
          Math.min(1, 0.06 + v[0] * 0.96),
          Math.min(1, 0.06 + v[1] * 0.96),
          Math.min(1, 0.06 + v[2] * 0.96)
        ];
      }
      /* Neutral steel keeps the arm readable without turning it into a toy. */
      const c = mute(seg.color);
      const l = c[0] * 0.2126 + c[1] * 0.7152 + c[2] * 0.0722;
      return [0.20 + l * 0.55, 0.23 + l * 0.58, 0.26 + l * 0.62];
    }

    /* Amber cage marks the selected link without flooding the solid. */
    function drawSelectionBox(link) {
      if (!link || !link.model) return;
      let mn = [-0.5, -0.5, -0.5];
      let mx = [0.5, 0.5, 0.5];
      if (link.mesh && link.mesh.bounds) {
        mn = link.mesh.bounds.min;
        mx = link.mesh.bounds.max;
      }
      const corners = [];
      const pad = [(mx[0] - mn[0]) * 0.02, (mx[1] - mn[1]) * 0.02, (mx[2] - mn[2]) * 0.02];
      for (let i = 0; i < 8; i++) {
        const c = [
          ((i & 1) ? mx[0] + pad[0] : mn[0] - pad[0]),
          ((i & 2) ? mx[1] + pad[1] : mn[1] - pad[1]),
          ((i & 4) ? mx[2] + pad[2] : mn[2] - pad[2])
        ];
        const w = M.transformPoint(link.model, c);
        corners.push(w);
      }
      const edges = [
        [0, 1], [1, 3], [3, 2], [2, 0],
        [4, 5], [5, 7], [7, 6], [6, 4],
        [0, 4], [1, 5], [2, 6], [3, 7]
      ];
      const pts = [];
      edges.forEach(function (e) {
        pts.push(corners[e[0]][0], corners[e[0]][1], corners[e[0]][2]);
        pts.push(corners[e[1]][0], corners[e[1]][1], corners[e[1]][2]);
      });
      gl.disable(gl.DEPTH_TEST);
      buffers.drawSegments(pts, currentVP, [0.84, 0.62, 0.33], 0.62);
      gl.enable(gl.DEPTH_TEST);
    }

    function jointRadius(scene, i) {
      const link = scene.links[i];
      let cross = link && link.seg && link.seg.width ? link.seg.width : 0.06;
      if (link && link.mesh && link.mesh.bounds && link.mesh.bounds.size) {
        const dims = link.mesh.bounds.size
          .filter(function (v) { return v > 1e-4; })
          .sort(function (a, b) { return a - b; });
        cross = dims.length > 1 ? dims[1] : (dims[0] || cross);
      }
      return Math.max(0.018, Math.min(0.045, cross * 0.75));
    }

    /* The arc needs the current view matrix; set by draw() before joints. */
    let currentVP = null;

    function drawJoint(joint, radius, selected, native) {
      const axis = joint.axis || [0, 0, 1];
      const align = alignZ(axis);
      const base = M.mul(M.trans(joint.pos[0], joint.pos[1], joint.pos[2]), align);

      const housing = M.mul(base, M.scale(radius * 1.55, radius * 1.55, radius * 0.95));
      buffers.drawMesh(buffers.cylMesh, housing, M.mat3(base), [0.12, 0.145, 0.17]);

      const ring = M.mul(base, M.scale(radius, radius, radius));
      const ringColor = selected ? [0.85, 0.61, 0.31] : [0.22, 0.30, 0.36];
      buffers.drawMesh(buffers.ringMesh, ring, M.mat3(base), ringColor);

      if (selected) {
        const axisModel = M.mul(base, M.scale(radius * 0.08, radius * 0.08, radius * 1.10));
        buffers.drawMesh(buffers.axisMesh, axisModel, M.mat3(base), axisColor(axis));
      }

      if (selected) {
        const pts = [];
        const basis = axisBasis(axis);
        const q = Math.max(-Math.PI * 2, Math.min(Math.PI * 2, joint.q || 0));
        const seg = 30;
        for (let i = 0; i <= seg; i++) {
          const a = q * i / seg;
          const c = Math.cos(a) * radius * 1.18;
          const s = Math.sin(a) * radius * 1.18;
          pts.push(
            joint.pos[0] + basis.u[0] * c + basis.v[0] * s,
            joint.pos[1] + basis.u[1] * c + basis.v[1] * s,
            joint.pos[2] + basis.u[2] * c + basis.v[2] * s
          );
        }
        buffers.drawPolyline(pts, currentVP, [0.92, 0.68, 0.36], 0.9);
        useMesh(currentVP);
      }
    }

    function drawShadow(scene, vp, native, doc) {
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
      gl.depthMask(false);
      let model;
      if (native) {
        model = M.mul(M.trans(0, 0.004, 0), M.scale(doc.base.w * 1.2, 1, doc.base.d * 1.2));
      } else {
        const b = F.render.sceneBounds(scene);
        if (!b) {
          gl.depthMask(true);
          gl.disable(gl.BLEND);
          return;
        }
        const r = Math.max(0.18, b.span * 0.62);
        model = M.mul(M.trans(b.center[0], 0.004, b.center[2]), M.scale(r, 1, r));
      }
      buffers.drawShadow(model, vp, native ? 0.42 : 0.34);
      gl.depthMask(true);
      gl.disable(gl.BLEND);
    }

    function drawNative(doc, scene, selected) {
      const b = doc.base;
      buffers.drawMesh(buffers.boxMesh,
        M.mul(M.trans(0, b.h / 2, 0), M.scale(b.w, b.h, b.d)),
        M.mat3(M.mat4()), [0.22, 0.25, 0.28]);
      buffers.drawMesh(buffers.cylMesh,
        M.mul(M.trans(0, b.h + 0.018, 0), M.scale(b.w * 0.52, 0.035, b.d * 0.52)),
        M.mat3(M.mat4()), [0.30, 0.34, 0.37]);

      [-0.30, 0.30].forEach(function (x) {
        [-0.20, 0.20].forEach(function (z) {
          buffers.drawMesh(buffers.plainBox,
            M.mul(M.trans(x * b.w, 0.022, z * b.d), M.scale(b.w * 0.16, 0.044, b.d * 0.16)),
            M.mat3(M.mat4()), [0.10, 0.12, 0.14]);
        });
      });

      for (let i = 0; i < scene.links.length; i++) {
        const l = scene.links[i];
        const sel = selected.kind === "joint" && selected.index === i;
        drawBuffer(buffers.boxMesh, l.model, linkColor(l.seg, i, sel, doc));
        if (sel) drawSelectionBox(l);
      }

      if (doc.prop && !doc.prop.held) {
        const p = doc.prop;
        const size = p.size || [0.12, 0.12, 0.12];
        buffers.drawMesh(buffers.boxMesh,
          M.mul(M.trans(p.pos[0], p.pos[1], p.pos[2]),
            M.scale(size[0], size[1], size[2])),
          M.mat3(M.mat4()), p.color || [0.78, 0.42, 0.18]);
      }

      const tool = scene.endMat;
      drawBuffer(
        buffers.boxMesh,
        M.mul(M.mul(tool, M.trans(doc.tool.length / 2, 0, 0)), M.scale(doc.tool.length, doc.tool.width, doc.tool.depth)),
        [0.30, 0.34, 0.38]
      );
      const grip = M.clamp(Number(doc.tool.grip) || 0, 0, 1);
      [0.065, -0.065].forEach(function (y) {
        const jaw = y * (1 - grip * 0.62);
        drawBuffer(
          buffers.boxMesh,
          M.mul(M.mul(tool, M.trans(doc.tool.length + 0.075 - grip * 0.025, jaw, 0)),
            M.scale(0.14, 0.030, 0.04)),
          [0.55, 0.59, 0.63]
        );
      });

      if (doc.prop && doc.prop.held) {
        const p = doc.prop;
        const size = p.size || [0.12, 0.12, 0.12];
        buffers.drawMesh(buffers.boxMesh,
          M.mul(M.mul(tool, M.trans(doc.tool.length + size[0] / 2, 0, 0)),
            M.scale(size[0], size[1], size[2])),
          M.mat3(M.mat4()), p.color || [0.78, 0.42, 0.18]);
      }

      for (let i = 0; i < scene.joints.length; i++) {
        if (nestedSpin(scene, i)) continue;
        drawJoint(scene.joints[i], jointRadius(scene, i), selected.kind === "joint" && selected.index === i, true);
      }
    }

    function drawFixture(scene) {
      const j = scene.joints[0];
      if (!j) return;
      const h = Math.max(0.03, j.pos[1]);
      const r = Math.max(0.06, jointRadius(scene, 0) * 1.65);
      buffers.drawMesh(buffers.boxMesh,
        M.mul(M.trans(j.pos[0], 0.022, j.pos[2]), M.scale(r * 2.4, 0.044, r * 2.4)),
        M.mat3(M.mat4()), [0.14, 0.17, 0.20]);
      buffers.drawMesh(buffers.boxMesh,
        M.mul(M.trans(j.pos[0], h / 2, j.pos[2]), M.scale(r * 1.55, h, r * 1.55)),
        M.mat3(M.mat4()), [0.16, 0.19, 0.22]);
      buffers.drawMesh(buffers.boxMesh,
        M.mul(M.trans(j.pos[0], h + 0.012, j.pos[2]), M.scale(r * 2.1, 0.024, r * 2.1)),
        M.mat3(M.mat4()), [0.26, 0.30, 0.34]);
    }

    function nestedSpin(scene, i) {
      const link = scene.links[i];
      const seg = link && link.seg;
      if (!seg || seg.parent == null || seg.parent < 0) return false;
      const parent = scene.links[seg.parent] && scene.links[seg.parent].seg;
      return !!(parent && parent.jointType === "steer" && seg.jointType === "spin");
    }

    function drawBody(doc, scene, selected) {
      if (doc.base && doc.base.hidden && doc.modelKind !== "vehicle") drawFixture(scene);
      for (let i = 0; i < scene.links.length; i++) {
        const l = scene.links[i];
        const sel = selected.kind === "joint" && selected.index === i;
        if (!l.mesh) continue;
        drawData(l.mesh, l.model, linkColor(l.seg, i, sel, doc));
        if (sel) drawSelectionBox(l);
      }
      for (let i = 0; i < scene.joints.length; i++) {
        const link = scene.links[i];
        if (!link || !link.seg || !link.seg.actuated) continue;
        if (nestedSpin(scene, i)) continue;
        const sel = selected.kind === "joint" && selected.index === i;
        if (doc.modelKind === "vehicle" && !sel) continue;
        drawJoint(scene.joints[i], jointRadius(scene, i), sel, false);
      }
    }

    function draw(doc, scene, vp, selected, eye) {
      resize();
      currentVP = vp;
      buffers.setEye(eye || [0, 0, 3]);

      gl.clearColor(0.02, 0.026, 0.032, 1);
      gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
      gl.disable(gl.CULL_FACE);
      gl.disable(gl.BLEND);

      gl.disable(gl.DEPTH_TEST);
      gl.depthMask(false);
      buffers.drawBg(canvas.width / Math.max(1, canvas.height));
      gl.enable(gl.DEPTH_TEST);
      gl.depthFunc(gl.LEQUAL);
      gl.depthMask(true);

      if (options.grid) buffers.drawGrid(vp);
      drawShadow(scene, vp, scene.mode !== "body", doc);

      useMesh(vp);
      if (scene.mode === "body") drawBody(doc, scene, selected);
      else drawNative(doc, scene, selected);

      labels.update(scene, vp, selected);
    }

    function setOptions(patch) {
      Object.keys(patch || {}).forEach(function (k) { options[k] = patch[k]; });
    }

    return {
      canvas: canvas,
      gl: gl,
      resize: resize,
      draw: draw,
      setOptions: setOptions,
      options: options
    };
  }

  F.render.createRenderer = createRenderer;
})(window.AEVUM.FORGE);
