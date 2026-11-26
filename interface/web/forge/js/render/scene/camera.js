/* Camera state, projection and smooth transitions. Input lives in app/workbench.js. */
(function (F) {
  "use strict";

  const D2R = F.math.D2R;
  const FOV = 40 * D2R;

  function createCamera() {
    return {
      yaw: 0.58, pitch: 0.30, dist: 2.1, target: [0.42, 0.32, 0],
      goalYaw: 0.58, goalPitch: 0.30, goalDist: 2.1, goalTarget: [0.42, 0.32, 0],
      ortho: false
    };
  }

  function cameraEye(cam) {
    const cp = Math.cos(cam.pitch);
    const sp = Math.sin(cam.pitch);
    return [
      cam.target[0] + cam.dist * cp * Math.sin(cam.yaw),
      cam.target[1] + cam.dist * sp,
      cam.target[2] + cam.dist * cp * Math.cos(cam.yaw)
    ];
  }

  function cameraBasis(cam) {
    const eye = cameraEye(cam);
    let fx = cam.target[0] - eye[0], fy = cam.target[1] - eye[1], fz = cam.target[2] - eye[2];
    const fl = Math.hypot(fx, fy, fz) || 1;
    fx /= fl; fy /= fl; fz /= fl;
    let rx = fz, ry = 0, rz = -fx;
    const rl = Math.hypot(rx, ry, rz) || 1;
    rx /= rl; ry /= rl; rz /= rl;
    const ux = ry * fz - rz * fy;
    const uy = rz * fx - rx * fz;
    const uz = rx * fy - ry * fx;
    return { eye: eye, forward: [fx, fy, fz], right: [rx, ry, rz], up: [ux, uy, uz] };
  }

  function ortho(height, aspect, near, far) {
    const m = new Float32Array(16);
    m[0] = 2 / (height * aspect);
    m[5] = 2 / height;
    m[10] = -2 / (far - near);
    m[14] = -(far + near) / (far - near);
    m[15] = 1;
    return m;
  }

  function viewProjection(cam, canvas) {
    const aspect = canvas.width / Math.max(1, canvas.height);
    const look = F.math.lookAt(cameraEye(cam), cam.target, [0, 1, 0]);
    if (cam.ortho) {
      return F.math.mul(ortho(cam.dist * 0.58, aspect, 0.05, 80), look);
    }
    return F.math.mul(F.math.perspective(FOV, aspect, 0.06, 60), look);
  }

  function lerp(a, b, t) {
    return a + (b - a) * t;
  }

  function step(cam, now) {
    const last = cam.last || now;
    const dt = Math.min(0.05, Math.max(0.001, (now - last) / 1000));
    cam.last = now;
    const k = 1 - Math.exp(-dt * 14);
    cam.yaw = lerp(cam.yaw, cam.goalYaw, k);
    cam.pitch = lerp(cam.pitch, cam.goalPitch, k);
    cam.dist = lerp(cam.dist, cam.goalDist, k);
    cam.target[0] = lerp(cam.target[0], cam.goalTarget[0], k);
    cam.target[1] = lerp(cam.target[1], cam.goalTarget[1], k);
    cam.target[2] = lerp(cam.target[2], cam.goalTarget[2], k);
  }

  function frameAt(cam, center, radius) {
    cam.goalTarget = [center[0], center[1], center[2]];
    cam.goalDist = Math.max(0.55, radius * 1.55);
  }

  function fitCamera(doc, cam, immediate) {
    const scene = F.render.buildScene(doc);
    let mnx = Infinity, mny = Infinity, mnz = Infinity, mxx = -Infinity, mxy = -Infinity, mxz = -Infinity;
    scene.links.forEach(function (l) {
      const p = F.math.translation(l.model);
      const b = l.mesh && l.mesh.bounds && l.mesh.bounds.size
        ? l.mesh.bounds.size
        : [Math.max(0.08, l.seg.length || 0.1), Math.max(0.04, l.seg.width || 0.06), Math.max(0.04, l.seg.depth || 0.06)];
      for (let c = 0; c < 3; c++) {
        const lo = p[c] - b[c] / 2;
        const hi = p[c] + b[c] / 2;
        if (c === 0) { mnx = Math.min(mnx, lo); mxx = Math.max(mxx, hi); }
        if (c === 1) { mny = Math.min(mny, lo); mxy = Math.max(mxy, hi); }
        if (c === 2) { mnz = Math.min(mnz, lo); mxz = Math.max(mxz, hi); }
      }
    });
    const end = scene.end || [0, 0, 0];
    const tool = doc.tool;
    const tip = tool && scene.endMat
      ? F.math.transformPoint(scene.endMat, [tool.length || 0, 0, 0])
      : end;
    mnx = Math.min(mnx, end[0], tip[0]); mxx = Math.max(mxx, end[0], tip[0]);
    mny = Math.min(mny, end[1], tip[1]); mxy = Math.max(mxy, end[1], tip[1]);
    mnz = Math.min(mnz, end[2], tip[2]); mxz = Math.max(mxz, end[2], tip[2]);
    if (doc.prop) {
      const p = doc.prop;
      const size = p.size || [0.12, 0.12, 0.12];
      const center = p.held && tool && scene.endMat
        ? F.math.transformPoint(scene.endMat, [Number(tool.length || 0) + size[0] / 2, 0, 0])
        : p.pos;
      mnx = Math.min(mnx, center[0] - size[0] / 2); mxx = Math.max(mxx, center[0] + size[0] / 2);
      mny = Math.min(mny, center[1] - size[1] / 2); mxy = Math.max(mxy, center[1] + size[1] / 2);
      mnz = Math.min(mnz, center[2] - size[2] / 2); mxz = Math.max(mxz, center[2] + size[2] / 2);
    }
    if (doc.base && !doc.base.hidden) {
      mnx = Math.min(mnx, -doc.base.w / 2); mxx = Math.max(mxx, doc.base.w / 2);
      mny = Math.min(mny, 0); mxy = Math.max(mxy, doc.base.h);
      mnz = Math.min(mnz, -doc.base.d / 2); mxz = Math.max(mxz, doc.base.d / 2);
    }
    if (!isFinite(mnx)) return;
    const cx = (mnx + mxx) / 2, cy = (mny + mxy) / 2, cz = (mnz + mxz) / 2;
    const span = Math.max(mxx - mnx, mxy - mny, mxz - mnz, 0.5);
    cam.goalTarget = [cx, cy, cz];
    cam.goalDist = Math.max(1.1, span * 1.9);
    if (immediate) {
      cam.target = [cx, cy, cz];
      cam.dist = cam.goalDist;
    }
  }

  function setView(cam, kind, doc) {
    if (kind === "home") {
      fitCamera(doc, cam);
      return;
    }
    if (kind === "top") {
      cam.goalPitch = 1.40;
      cam.goalYaw = 0.02;
    } else if (kind === "front") {
      cam.goalPitch = 0.10;
      cam.goalYaw = 0;
    } else if (kind === "right") {
      cam.goalPitch = 0.10;
      cam.goalYaw = Math.PI / 2;
    }
  }

  function pan(cam, dx, dy, viewportH) {
    const b = cameraBasis(cam);
    const scale = 2 * cam.dist * Math.tan(FOV / 2) / Math.max(1, viewportH);
    const tx = -b.right[0] * dx * scale + b.up[0] * dy * scale;
    const ty = -b.right[1] * dx * scale + b.up[1] * dy * scale;
    const tz = -b.right[2] * dx * scale + b.up[2] * dy * scale;
    cam.goalTarget[0] += tx;
    cam.goalTarget[1] += ty;
    cam.goalTarget[2] += tz;
    cam.target[0] += tx;
    cam.target[1] += ty;
    cam.target[2] += tz;
  }

  F.render.createCamera = createCamera;
  F.render.cameraEye = cameraEye;
  F.render.cameraBasis = cameraBasis;
  F.render.viewProjection = viewProjection;
  F.render.cameraStep = step;
  F.render.cameraFrameAt = frameAt;
  F.render.fitCamera = fitCamera;
  F.render.setCameraView = setView;
  F.render.panCamera = pan;
})(window.AEVUM.FORGE);
