/* Camera state, projection and smooth transitions. Input lives in app/workbench.js. */
(function (F) {
  "use strict";

  const D2R = F.math.D2R;
  const FOV = 40 * D2R;

  function createCamera() {
    return {
      yaw: 0.58, pitch: 0.30, dist: 2.1, target: [0.42, 0.32, 0],
      goalYaw: 0.58, goalPitch: 0.30, goalDist: 2.1, goalTarget: [0.42, 0.32, 0],
      mode: "orbit", eye: null,
      ortho: false
    };
  }

  function cameraEye(cam) {
    if (cam.mode === "fixed" && cam.eye) return cam.eye.slice();
    if ((cam.mode === "first" || cam.mode === "follow") && cam.eye) return cam.eye.slice();
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
    if (cam.mode === "first" || cam.mode === "follow" || cam.mode === "fixed") return;
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
    const bounds = F.render.sceneBounds(scene);
    let mnx = bounds ? bounds.min[0] : Infinity;
    let mny = bounds ? bounds.min[1] : Infinity;
    let mnz = bounds ? bounds.min[2] : Infinity;
    let mxx = bounds ? bounds.max[0] : -Infinity;
    let mxy = bounds ? bounds.max[1] : -Infinity;
    let mxz = bounds ? bounds.max[2] : -Infinity;
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

  /*
   * 一条有界路径整段入画：相机目标落在路径中点，距离覆盖线段和车体。
   * 页面只调整呈现，不改变内核路径或车辆位姿。
   */
  function framePath(cam, doc, ax, az, bx, bz, radius) {
    const scene = F.render.buildScene(doc);
    const bounds = F.render.sceneBounds(scene);
    const pose = doc.pose || { x: 0, z: 0 };
    const localX = bounds ? (bounds.min[0] + bounds.max[0]) / 2 - pose.x : 0;
    const localZ = bounds ? (bounds.min[2] + bounds.max[2]) / 2 - pose.z : 0;
    const cx = (ax + bx) / 2 + localX;
    const cz = (az + bz) / 2 + localZ;
    const length = Math.hypot(bx - ax, bz - az);
    const span = bounds
      ? Math.max(bounds.max[0] - bounds.min[0], bounds.max[1] - bounds.min[1],
                 bounds.max[2] - bounds.min[2], 0.5)
      : Math.max(0.5, radius || 0.12);
    cam.goalTarget[0] = cx;
    cam.goalTarget[2] = cz;
    if (bounds) cam.goalTarget[1] = (bounds.min[1] + bounds.max[1]) / 2;
    cam.goalDist = Math.max(1.35, length * 1.4 + span * 1.4);
  }

  function frameArc(cam, doc, cx, cz, r) {
    const scene = F.render.buildScene(doc);
    const bounds = F.render.sceneBounds(scene);
    const span = bounds
      ? Math.max(bounds.max[0] - bounds.min[0], bounds.max[1] - bounds.min[1],
                 bounds.max[2] - bounds.min[2], 0.5)
      : 0.5;
    cam.goalTarget[0] = cx;
    cam.goalTarget[2] = cz;
    if (bounds) cam.goalTarget[1] = (bounds.min[1] + bounds.max[1]) / 2;
    cam.goalDist = Math.max(2.8, (Math.abs(r) * 2 + span) * 2.2);
  }

  function carBasis(yaw) {
    return {
      fx: Math.cos(yaw), fz: -Math.sin(yaw),
      rx: Math.sin(yaw), rz: Math.cos(yaw)
    };
  }

  function setMode(cam, mode, doc) {
    if (mode === "fixed") {
      /* 固定跟随：记录当前机位相对车体的 along/side/height，
       * 之后车怎么走，相机保持这个相对位置跟过去。 */
      const pose = doc && doc.pose;
      if (pose) {
        const b = carBasis(Number(pose.yaw) || 0);
        const eye = cameraEye(cam);
        const target = cam.target;
        const rel = function (p) {
          const dx = p[0] - (Number(pose.x) || 0);
          const dz = p[2] - (Number(pose.z) || 0);
          return {
            along: dx * b.fx + dz * b.fz,
            side: dx * b.rx + dz * b.rz,
            y: p[1]
          };
        };
        cam.fixed = { eye: rel(eye), target: rel(target) };
        cam.eye = eye;
        cam.target = target.slice();
      }
      cam.mode = "fixed";
      return;
    }
    cam.fixed = null;
    cam.mode = mode === "first" ? "first" : (mode === "follow" ? "follow" : "orbit");
    if (cam.mode === "orbit") cam.eye = null;
  }

  /* 车相机：first 是车头眼位，follow 是车后跟随；都直接读车体 pose。 */
  function followCar(cam, doc) {
    if (!cam || !doc) return;
    if (cam.mode === "fixed") {
      const off = cam.fixed;
      if (!off) return;
      const pose = doc.pose || { x: 0, z: 0, yaw: 0 };
      const b = carBasis(Number(pose.yaw) || 0);
      cam.eye = [
        (Number(pose.x) || 0) + b.fx * off.eye.along + b.rx * off.eye.side,
        off.eye.y,
        (Number(pose.z) || 0) + b.fz * off.eye.along + b.rz * off.eye.side
      ];
      cam.target = [
        (Number(pose.x) || 0) + b.fx * off.target.along + b.rx * off.target.side,
        off.target.y,
        (Number(pose.z) || 0) + b.fz * off.target.along + b.rz * off.target.side
      ];
      return;
    }
    if (cam.mode !== "first" && cam.mode !== "follow") return;
    const pose = doc.pose || { x: 0, z: 0, yaw: 0 };
    const yaw = Number(pose.yaw) || 0;
    const fx = Math.cos(yaw);
    /* axisAngle(+Y) 把局部 +X 转到世界 -Z，车相机也用同一朝向。 */
    const fz = -Math.sin(yaw);
    const rx = Math.sin(yaw);
    const rz = Math.cos(yaw);
    if (cam.mode === "first") {
      const seatX = 0.17;
      const seatZ = 0.03;
      cam.eye = [
        (Number(pose.x) || 0) + fx * seatX + rx * seatZ,
        0.15,
        (Number(pose.z) || 0) + fz * seatX + rz * seatZ
      ];
      cam.target = [
        cam.eye[0] + fx * 0.9,
        0.105,
        cam.eye[2] + fz * 0.9
      ];
      return;
    }
    /* 跟随第三人称：在车后上方看车，目标略在车头前方。 */
    const back = 1.15;
    const side = 0.06;
    cam.eye = [
      (Number(pose.x) || 0) - fx * back + rx * side,
      0.42,
      (Number(pose.z) || 0) - fz * back + rz * side
    ];
    cam.target = [
      (Number(pose.x) || 0) + fx * 0.12,
      0.10,
      (Number(pose.z) || 0) + fz * 0.12
    ];
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
  F.render.cameraFollow = followCar;
  F.render.cameraSetMode = setMode;
  F.render.cameraFrameAt = frameAt;
  F.render.fitCamera = fitCamera;
  F.render.framePath = framePath;
  F.render.frameArc = frameArc;
  F.render.setCameraView = setView;
  F.render.panCamera = pan;
})(window.AEVUM.FORGE);
