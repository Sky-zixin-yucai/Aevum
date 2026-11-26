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

    function q16(v) {
      return (Number(v) || 0) / Q16;
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
          toast("当前模型不接内核：" + compat.reason + "；仅本地建模/仿真");
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
      }

      q.forEach(function (v, i) {
        if (state.doc.segments[i]) {
          state.doc.segments[i].q = v;
          if (!state.sim.manual && !state.sim.demoDrive) state.doc.segments[i].target = qt[i];
          state.sim.q[i] = v;
          state.sim.qd[i] = dq[i];
        }
      });
      state.sim.kernel = true;
      state.sim.running = false;
      state.sim.sine = false;
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
        state.sim.running = false;
        state.sim.sine = false;
        state.sim.qHist.push(message.q.slice());
        if (state.sim.qHist.length > 360) state.sim.qHist.shift();
        if (!wasKernel) {
          setKernelPill("kernel");
          syncControls();
          onKernelState();
          toast("Kernel q 已接入；本地 PD 已暂停");
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

    function setManual(on) {
      if (!state.sim.kernel) return;
      state.sim.manual = !!on;
      state.sim.manualSince = performance.now();
      if (state.sim.manual) {
        /* 从 AUTO 进入手动时先锁当前 q，避免跳到上一条 q_target。 */
        state.doc.segments.forEach(function (s) { s.target = s.q; });
        sendManual();
        toast("MANUAL 保持已开启；释放后立即回 AUTO，桥接断开时 5 秒兜底");
      } else {
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
      syncControls();
    }

    function resetLink() {
      setKernelPill(false);
      state.sim.kernel = false;
      state.sim.manual = false;
      state.sim.manualSince = 0;
      state.sim.bodyPending = false;
      state.sim.kernelStatus = null;
      state.sim.running = false;
      state.sim.sine = false;
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
          toast("Bridge 不可达；本地仿真继续" + (e && e.message ? "（" + e.message + "）" : ""));
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
      setManual: setManual,
      pulseManual: pulseManual
    };
  }

  F.app = F.app || {};
  F.app.createBridgeControl = createBridgeControl;
})(window.AEVUM.FORGE);
