/* 通用平面关节链键盘目标：W/A/S/D 只产生末端位移，q_target 交给内核执行。 */
(function (F) {
  "use strict";

  F.app = F.app || {};

  F.app.createKeyboardControl = function (opts) {
    const state = opts.state;
    const bridgeControl = opts.bridgeControl;
    const toast = opts.toast || function () {};
    const armKeys = {
      w: [0, 1],
      s: [0, -1],
      a: [-1, 0],
      d: [1, 0]
    };
    /* 车辆/导入模型用游戏式操作：W/S 沿车头前后，A/D 原地打方向。 */
    const carKeys = {
      w: { move: 1, turn: 0 },
      s: { move: -1, turn: 0 },
      a: { move: 0, turn: -1 },
      d: { move: 0, turn: 1 }
    };
    const STEP = 0.035;
    const STEER = 0.12;
    const DAMP = 0.08;
    const MAX_DQ = 0.18;
    const MAX_DRIVE_DQ = 0.10;
    let lastToast = 0;
    const steerHeld = { a: false, d: false };
    const driveHeld = { w: false, s: false };

    function keysForDoc() {
      const doc = state.doc;
      return doc && doc.modelKind !== "arm" ? carKeys : armKeys;
    }

    function notice(text) {
      const now = performance.now();
      if (now - lastToast < 1200) return;
      lastToast = now;
      toast(text);
    }

    function controlDoc() {
      return state.control || state.doc;
    }

    function chain() {
      return F.model.controlSegments(controlDoc());
    }

    function lengths(segs) {
      return segs.map(function (s) { return F.model.controlLength(controlDoc(), s); });
    }

    function currentQ(segs) {
      return segs.map(function (s) {
        const v = state.sim.manual ? s.target : s.q;
        return Number.isFinite(v) ? v : (Number(s.q) || 0);
      });
    }

    /*
     * 2×N 平面雅可比：末端 x/y 对每个关节的偏导。
     * 只用阻尼伪逆把一次按键的末端位移映射成 q_target 增量；
     * 真正的速度、限幅、PD 和保持都由内核完成，页面不积分。
     */
    function mapTipDelta(segs, q, dx, dy) {
      const L = lengths(segs);
      const theta = [];
      let sum = 0;
      q.forEach(function (v, i) {
        sum += v;
        theta[i] = sum;
      });
      const jx = new Array(segs.length).fill(0);
      const jy = new Array(segs.length).fill(0);
      let sx = 0;
      let sy = 0;
      for (let i = segs.length - 1; i >= 0; i--) {
        sx += L[i] * Math.sin(theta[i]);
        sy += L[i] * Math.cos(theta[i]);
        jx[i] = -sx;
        jy[i] = sy;
      }
      const a = jx.reduce(function (n, v) { return n + v * v; }, 0) + DAMP;
      const b = jx.reduce(function (n, v, i) { return n + v * jy[i]; }, 0);
      const c = jy.reduce(function (n, v) { return n + v * v; }, 0) + DAMP;
      const det = a * c - b * b;
      if (Math.abs(det) < 1e-8) return q.slice();
      const u = (c * dx - b * dy) / det;
      const v = (-b * dx + a * dy) / det;
      return q.map(function (_, i) {
        return q[i] + F.math.clamp(jx[i] * u + jy[i] * v, -MAX_DQ, MAX_DQ);
      });
    }

    /*
     * 车辆需要“边前进边保持朝向”或“原地转向”，所以用 3×N 的
     * (x, y, yaw) 雅可比；3R 正好 3 个关节，可以同时约束位置与车头角。
     */
    function mapTipPose(segs, q, dx, dy, dphi, maxDq) {
      const L = lengths(segs);
      const theta = [];
      let sum = 0;
      q.forEach(function (v, i) {
        sum += v;
        theta[i] = sum;
      });
      const jx = new Array(segs.length).fill(0);
      const jy = new Array(segs.length).fill(0);
      let sx = 0;
      let sy = 0;
      for (let i = segs.length - 1; i >= 0; i--) {
        sx += L[i] * Math.sin(theta[i]);
        sy += L[i] * Math.cos(theta[i]);
        jx[i] = -sx;
        jy[i] = sy;
      }
      let a00 = DAMP, a01 = 0, a02 = 0, a11 = DAMP, a12 = 0, a22 = DAMP;
      jx.forEach(function (vx, i) {
        const vy = jy[i];
        a00 += vx * vx;
        a01 += vx * vy;
        a02 += vx;
        a11 += vy * vy;
        a12 += vy;
        a22 += 1;
      });
      /* 3×3 对称矩阵求逆，解 J^T λ 的阻尼最小二乘。 */
      const det = a00 * (a11 * a22 - a12 * a12)
        - a01 * (a01 * a22 - a12 * a02)
        + a02 * (a01 * a12 - a11 * a02);
      if (Math.abs(det) < 1e-8) return q.slice();
      const i00 = (a11 * a22 - a12 * a12) / det;
      const i01 = (a02 * a12 - a01 * a22) / det;
      const i02 = (a01 * a12 - a02 * a11) / det;
      const i11 = (a00 * a22 - a02 * a02) / det;
      const i12 = (a01 * a02 - a00 * a12) / det;
      const i22 = (a00 * a11 - a01 * a01) / det;
      const u = i00 * dx + i01 * dy + i02 * dphi;
      const v = i01 * dx + i11 * dy + i12 * dphi;
      const w = i02 * dx + i12 * dy + i22 * dphi;
      return q.map(function (_, i) {
        return q[i] + F.math.clamp(jx[i] * u + jy[i] * v + w, -maxDq, maxDq);
      });
    }

    function ensureReady(segs) {
      if (!state.bridge.online) {
        notice("先连接 0x03 内核桥接。");
        return false;
      }
      if (!segs.length) {
        notice("WASD 需要内核 3R 控制链；当前没有可控关节。");
        return false;
      }
      if (!state.sim.kernel) {
        notice("等待内核 0x03；当前还没有 q 状态。");
        return false;
      }
      return true;
    }

    function applyNext(segs, next) {
      segs.forEach(function (s, i) {
        const lo = Math.min(Number(s.min) || -180, Number(s.max) || 180) * F.math.D2R;
        const hi = Math.max(Number(s.min) || -180, Number(s.max) || 180) * F.math.D2R;
        s.target = F.math.clamp(next[i], lo, hi);
      });
      if (!bridgeControl.pulseManual()) {
        notice("内核链路未就绪，WASD 未发出。");
      }
    }

    function jogArm(dx, dy) {
      const segs = chain();
      if (!ensureReady(segs)) return;
      const q = currentQ(segs);
      applyNext(segs, mapTipDelta(segs, q, dx * STEP, dy * STEP));
    }

    function jogCar(move, turn) {
      const segs = chain();
      if (!ensureReady(segs)) return;
      const q = currentQ(segs);
      const pose = state.doc.pose || { x: 0, z: 0, yaw: 0 };

      /* 游戏式方向盘：A/D 只改变前轮转角，不再原地旋转车身，也不解锁车辆。 */
      if (turn) {
        const cur = Number(state.avatarSteer) || 0;
        state.avatarSteer = F.math.clamp(cur + turn * 0.16, -0.5, 0.5);
        if (opts.onCarSteer) opts.onCarSteer(pose, state.avatarSteer);
        return;
      }
      if (!move) return;
      /* 方向盘和油门互斥：不允许同一次按键同时产生移动和转向。 */
      if (steerHeld.a || steerHeld.d) {
        notice("先松开 A/D，再按 W/S。");
        return;
      }
      /* 键盘驾驶不应沿用上一次 path/弧的引导线，否则 W 看起来像被弧指令接管。 */
      if (state.doc) state.doc.guide = null;
      state.avatarDriveSign = move >= 0 ? 1 : -1;

      /* 方向盘角度保留到下一次 W/S：A/D 只设前轮，松开后不会自动回正；
       * 这样先打轮、再给油时 W/S 会按前轮转角走弧，而不是继续走直线。 */

      if (!state.avatarDriving) {
        /* 第一次前进/后退才解锁；先把内核目标锁在当前 q，
         * 避免旧 q_target 还在追，车一解锁就漂走。 */
        segs.forEach(function (s, i) { s.target = q[i]; });
        state.avatarDriving = true;
        state.avatarAnchor = state.controlTip ? state.controlTip() : null;
        state.avatarPose = { x: 0, z: 0, yaw: 0 };
      }

      /* 车辆显示/位移用游戏位姿积分；3R q 只是内核侧的外观跟随。
       * 车头、位置、方向盘在同一套 x/z/yaw 坐标里，W 就是沿车头直线。 */
      const car = state.avatarPose || (state.avatarPose = { x: 0, z: 0, yaw: 0 });
      const steer = Number(state.avatarSteer) || 0;
      const step = move * STEP;
      const yaw = Number(car.yaw) || 0;
      const startYaw = yaw;
      car.x += Math.cos(yaw) * step;
      car.z -= Math.sin(yaw) * step;
      /* 自行车模型：只有前进/后退时才由前轮转角改变车头，静止打方向盘车身不动。 */
      const wheelbase = 0.22;
      const dphi = move * (step / wheelbase) * Math.tan(steer);
      if (steer) car.yaw = yaw + dphi;
      if (state.doc) {
        state.doc.pose = car;
      }

      /* 同步给内核 3R 的 q_target：方向用本步开始时的车头，避免把新航向
       * 再叠加到本步位移上。 */
      const dx = Math.cos(startYaw) * step;
      const dy = -Math.sin(startYaw) * step;
      const next = mapTipPose(segs, q, dx, dy, dphi, MAX_DRIVE_DQ);
      applyNext(segs, next);
    }

    function onKeyDown(event) {
      if (event.ctrlKey || event.metaKey || event.altKey) return;
      const tag = event.target && event.target.tagName ? event.target.tagName.toLowerCase() : "";
      if (tag === "input" || tag === "textarea" || tag === "select") return;
      const key = (event.key || "").toLowerCase();
      const cmd = keysForDoc()[key];
      if (!cmd) return;
      event.preventDefault();
      if (key === "w" || key === "s") {
        /* 四键互斥：方向盘按住时不能给油，油门按住时不能打方向。
         * 这样 A/D 与 W/S 不会在同一帧里串成弧线。 */
        if (steerHeld.a || steerHeld.d) {
          notice("先松开 A/D，再按 W/S。");
          return;
        }
        driveHeld[key] = true;
      }
      if (key === "a" || key === "d") {
        if (driveHeld.w || driveHeld.s) {
          notice("行驶中不能打 A/D；先松开 W/S。");
          return;
        }
        if (steerHeld.a && key === "d" || steerHeld.d && key === "a") {
          notice("A 和 D 不能同时按。");
          return;
        }
        steerHeld[key] = true;
      }
      if (state.doc && state.doc.modelKind !== "arm") {
        jogCar(cmd.move, cmd.turn);
      } else {
        jogArm(cmd[0], cmd[1]);
      }
    }

    function onKeyUp(event) {
      const key = (event.key || "").toLowerCase();
      if (key === "a" || key === "d") steerHeld[key] = false;
      if (key === "w" || key === "s") driveHeld[key] = false;
    }

    window.addEventListener("keydown", onKeyDown, { passive: false });
    window.addEventListener("keyup", onKeyUp);
    window.addEventListener("blur", function () {
      steerHeld.a = false;
      steerHeld.d = false;
      driveHeld.w = false;
      driveHeld.s = false;
    });
    return { jog: jogArm, jogCar: jogCar };
  };
})(window.AEVUM.FORGE);
