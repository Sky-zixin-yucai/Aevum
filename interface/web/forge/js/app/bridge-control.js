/* Bridge control: WebSocket lifecycle, kernel status ingest, manual joint command. */
(function (F) {
  "use strict";

  const Q16 = 65536;

  function createBridgeControl(opts) {
    const state = opts.state;
    const setKernelPill = opts.setKernelPill || function () {};
    const rerenderInspector = opts.rerenderInspector || function () {};
    const syncControls = opts.syncControls || function () {};
    const onKernelState = opts.onKernelState || function () {};
    const updateKernelStatus = opts.updateKernelStatus || function () {};
    const toast = opts.toast || function () {};
    let manualHoldTimer = null;

    /*
     * 内核手动目标有 5 秒保持窗口。页面打开时每 2 秒重发当前目标，
     * 让 MANUAL HOLD 不会因为“没有再拖滑块”自动掉回 AUTO；
     * 只有页面/桥真正断开时才走 5 秒兜底。
     */
    function stopManualHold() {
      if (manualHoldTimer) {
        clearInterval(manualHoldTimer);
        manualHoldTimer = null;
      }
    }

    function startManualHold() {
      stopManualHold();
      manualHoldTimer = setInterval(function () {
        if (state.sim.kernel && state.sim.manual) sendManual();
      }, 2000);
    }

    function q16(v) {
      return (Number(v) || 0) / Q16;
    }

    function wheelRadius() {
      const wheels = state.doc && state.doc.aevum && state.doc.aevum.wheels;
      return wheels && wheels.length ? (Number(wheels[0].radius) || 0.04) : 0.04;
    }

    /*
     * 0x05 是内核几何路径状态，不是车辆状态：页面只把 (x,z)/切向/弧长
     * 当作被控对象位姿和轮子角度来呈现，不发命令、不积分路径。
     */
    function applyPath(message) {
      if (!state.doc || state.doc.modelKind !== "vehicle") return;
      const wasKernel = state.sim.kernel;
      const prevPath = state.sim.path;
      const wasState = prevPath ? prevPath.state : -1;
      state.sim.path = {
        kind: Number(message.kind) || 0,
        state: Number(message.state) || 0,
        fault: Number(message.fault) || 0,
        tick: Number(message.tick) || 0,
        s: Number(message.s) || 0,
        t: Number(message.t) || 0,
        x: Number(message.x) || 0,
        z: Number(message.z) || 0,
        dx: Number(message.dx) || 0,
        dz: Number(message.dz) || 0,
        speed: Number(message.speed) || 0,
        length: Number(message.length) || 0,
        at: performance.now()
      };

      const pose = state.doc.pose || (state.doc.pose = { x: 0, z: 0, yaw: 0 });
      pose.x = state.sim.path.x;
      pose.z = state.sim.path.z;
      pose.yaw = state.sim.path.dx === 0 && state.sim.path.dz === 0
        ? 0
        : Math.atan2(-state.sim.path.dz, state.sim.path.dx);

      const pathChanged = !prevPath ||
        prevPath.kind !== state.sim.path.kind ||
        prevPath.length !== state.sim.path.length;
      if (pathChanged && state.cam && (state.sim.path.state === 1 || state.sim.path.state === 2)) {
        const sx = state.sim.path.x - state.sim.path.dx * state.sim.path.s;
        const sz = state.sim.path.z - state.sim.path.dz * state.sim.path.s;
        F.render.framePath(
          state.cam,
          state.doc,
          sx, sz,
          sx + state.sim.path.dx * state.sim.path.length,
          sz + state.sim.path.dz * state.sim.path.length,
          0.30
        );
      }

      /* 滚动角只做呈现：-s/r 对应车体沿 +x 前进时轮子向前滚。 */
      const spin = -state.sim.path.s / wheelRadius();
      state.doc.segments.forEach(function (seg) {
        if (seg.jointType === "steer") seg.q = 0;
        else if (seg.jointType === "spin") seg.q = spin;
      });

      state.sim.kernel = true;
      if (!wasKernel) {
        setKernelPill("kernel");
        toast("Kernel LINE 已接入；路径点与滚角来自 0x05");
      }
      if (!wasKernel || wasState !== state.sim.path.state) {
        syncControls();
        rerenderInspector();
        onKernelState();
      }
      const line = document.getElementById("lineState");
      if (line) {
        const states = ["IDLE", "RUN", "DONE", "FAULT"];
        const stateName = states[state.sim.path.state] || "?";
        line.textContent = stateName +
          " · s " + state.sim.path.s.toFixed(3) + " / " + state.sim.path.length.toFixed(3) + " m" +
          " · t " + state.sim.path.t.toFixed(3);
      }
      updateKernelStatus();
    }

    function bodyPacket() {
      const compat = F.model.kernelCompatibility(state.doc);
      if (!compat.ok) return null;
      const body = F.model.bodyDescriptor(state.doc);
      return body ? { type: "body", body: body } : null;
    }

    function sendBody() {
      const packet = bodyPacket();
      if (!packet) return false;
      state.sim.bodyPending = true;
      state.sim.bodySentAt = performance.now();
      return F.bridge.send(state.bridge, packet, true);
    }

    function applyStatus(message) {
      const compat = F.model.kernelCompatibility(state.doc);
      if (!compat.ok) {
        if (!state.sim.kernelRejected) {
          state.sim.kernelRejected = true;
          toast("当前模型不接内核：" + compat.reason + "；仅模型呈现");
        }
        return;
      }
      state.sim.kernelRejected = false;
      const values = message.values || {};
      const n = Number(message.n) || 0;
      if (!n || !Array.isArray(message.q) || message.q.length !== n) return;
      if (state.doc.segments.length !== n) {
        const stale = !state.sim.bodyPending ||
          performance.now() - (state.sim.bodySentAt || 0) > 1500;
        if (stale) sendBody();
        return;
      }
      state.sim.bodyPending = false;
      const q = message.q;
      const dq = Array.isArray(message.dq) ? message.dq : q.map(function () { return 0; });
      const qt = Array.isArray(message.qTarget) ? message.qTarget : q.slice();
      const wasKernel = state.sim.kernel;
      const wasMode = state.sim.kernelStatus ? state.sim.kernelStatus.mode : -1;
      const mode = Number(values.mode) || 0;

      /* 内核已经回 AUTO，但本地还停在 MANUAL：超过一个状态周期就跟随内核。 */
      if (mode === 0 && state.sim.manual &&
          performance.now() - (state.sim.manualSince || 0) > 1500) {
        state.sim.manual = false;
        stopManualHold();
      }

      q.forEach(function (v, i) {
        if (state.doc.segments[i]) {
          state.doc.segments[i].q = v;
          if (!state.sim.manual) state.doc.segments[i].target = qt[i];
          state.sim.q[i] = v;
          state.sim.qd[i] = dq[i];
        }
      });
      state.sim.kernel = true;
      state.sim.kernelStatus = {
        n: n,
        q: q.slice(),
        dq: dq.slice(),
        qTarget: qt.slice(),
        target: [q16(values.tx), q16(values.ty)],
        errMm: q16(values.err_mm),
        wManip: q16(values.w_manip),
        sigmaMin: q16(values.sigma_min),
        sigmaMax: q16(values.sigma_max),
        nullShare: q16(values.null_share),
        limitMargin: q16(values.limit_margin),
        mode: mode,
        nullOn: Number(values.null_on) || 0,
        at: performance.now()
      };
      state.sim.qHist.push(q.slice());
      if (state.sim.qHist.length > 360) state.sim.qHist.shift();
      if (!wasKernel) {
        setKernelPill("kernel");
        toast("Kernel 完整状态已接入；" + n + " 个关节 · MANUAL 可下发目标");
      }
      syncControls();
      if (!wasKernel || wasMode !== mode) {
        rerenderInspector();
        onKernelState();
      }
      updateKernelStatus();
    }

    function applyMessage(message) {
      if (!message) return;
      if (message.type === "path") {
        applyPath(message);
        return;
      }
      if (message.type === "status" && message.values) {
        applyStatus(message);
        return;
      }
      if (Array.isArray(message.q) && message.q.length >= 2) {
        const wasKernel = state.sim.kernel;
        message.q.forEach(function (v, i) {
          if (state.doc.segments[i]) {
            state.doc.segments[i].q = v;
            state.sim.q[i] = v;
            state.sim.qd[i] = 0;
          }
        });
        state.sim.kernel = true;
        state.sim.qHist.push(message.q.slice());
        if (state.sim.qHist.length > 360) state.sim.qHist.shift();
        if (!wasKernel) {
          setKernelPill("kernel");
          syncControls();
          onKernelState();
          toast("Kernel q 已接入；页面控制已停用");
        }
        rerenderInspector();
      }
      if (Array.isArray(message.target)) {
        message.target.forEach(function (v, i) {
          if (state.doc.segments[i]) state.doc.segments[i].target = v;
        });
      }
    }

    function sendManual() {
      if (!state.sim.kernel) return false;
      return F.bridge.send(state.bridge, {
        type: "joint",
        q: state.doc.segments.map(function (s) { return +s.target.toFixed(5); })
      });
    }

    function sendRelease() {
      return F.bridge.send(state.bridge, { type: "release" }, true);
    }

    /* 外部动作实例参数；内核按几何生成器执行，页面不本地积分。 */
    function sendPath(params) {
      if (!state.bridge.online) return false;
      const packet = Object.assign({ type: "path", kind: 1 }, params || {});
      return F.bridge.send(state.bridge, packet);
    }

    function setManual(on) {
      if (!state.sim.kernel) return;
      state.sim.manual = !!on;
      state.sim.manualSince = performance.now();
      if (state.sim.manual) {
        /* 从 AUTO 进入手动时先锁当前 q，避免跳到上一条 q_target。 */
        state.doc.segments.forEach(function (s) { s.target = s.q; });
        sendManual();
        startManualHold();
        toast("MANUAL 保持已开启；页面每 2 秒续期，桥接断开时 5 秒兜底");
      } else {
        stopManualHold();
        sendRelease();
        toast("MANUAL 已释放；内核立即回 AUTO");
      }
      syncControls();
      rerenderInspector();
    }

    function pulseManual() {
      if (!state.sim.kernel) return;
      state.sim.manual = true;
      state.sim.manualSince = performance.now();
      sendManual();
      startManualHold();
      syncControls();
    }

    function resetLink() {
      stopManualHold();
      setKernelPill(false);
      state.sim.kernel = false;
      state.sim.manual = false;
      state.sim.manualSince = 0;
      state.sim.bodyPending = false;
      state.sim.kernelStatus = null;
      state.sim.path = null;
      if (state.doc) state.doc.pose = { x: 0, z: 0, yaw: 0 };
      state.sim.qHist.length = 0;
      syncControls();
      rerenderInspector();
    }

    function connect(url) {
      state.bridge.url = url || state.bridge.url;
      if (state.bridge.online) return;
      F.bridge.connect(state.bridge, {
        onOpen: function () {
          setKernelPill("bridge");
          syncControls();
          rerenderInspector();
          sendBody();
          toast("Bridge connected · body descriptor sent");
        },
        onClose: resetLink,
        onError: function (e) {
          toast("Bridge 不可达；页面仅做模型呈现" + (e && e.message ? "（" + e.message + "）" : ""));
        },
        onMessage: applyMessage
      });
    }

    function disconnect() {
      F.bridge.disconnect(state.bridge, { onClose: resetLink });
    }

    function toggle(url) {
      if (state.bridge.online) disconnect();
      else connect(url);
    }

    return {
      connect: connect,
      disconnect: disconnect,
      toggle: toggle,
      applyMessage: applyMessage,
      sendManual: sendManual,
      sendBody: sendBody,
      sendPath: sendPath,
      setManual: setManual,
      pulseManual: pulseManual
    };
  }

  F.app = F.app || {};
  F.app.createBridgeControl = createBridgeControl;
})(window.AEVUM.FORGE);
