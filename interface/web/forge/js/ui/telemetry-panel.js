/* Footer telemetry: model stats by default, kernel 0x03 state when the kernel is in the loop. */
(function (F) {
  "use strict";

  function createTelemetryPanel(opts) {
    const state = opts.state;
    const chart = opts.chart;
    const ctx = chart.getContext("2d");
    const onMetrics = opts.onMetrics || function () {};

    function resize() {
      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      const w = chart.clientWidth || 1;
      const h = chart.clientHeight || 1;
      if (chart.width !== Math.floor(w * dpr) || chart.height !== Math.floor(h * dpr)) {
        chart.width = Math.floor(w * dpr);
        chart.height = Math.floor(h * dpr);
      }
    }

    function label(id, text) {
      const el = F.dom.$(id);
      const m = el && el.parentElement && el.parentElement.querySelector(".m");
      if (m) m.textContent = text;
    }

    function drawGrid(w, h) {
      ctx.strokeStyle = "#1b242b";
      ctx.lineWidth = 1;
      for (let i = 1; i < 4; i++) {
        const y = h * i / 4;
        const x = w * i / 4;
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(w, y);
        ctx.moveTo(x, 0);
        ctx.lineTo(x, h);
        ctx.stroke();
      }
    }

    function drawChart() {
      resize();
      const w = chart.width;
      const h = chart.height;
      const dpr = Math.max(1, w / Math.max(1, chart.clientWidth || 1));
      const fs = Math.round(12 * dpr);
      ctx.clearRect(0, 0, w, h);
      ctx.fillStyle = "#0d1115";
      ctx.fillRect(0, 0, w, h);
      drawGrid(w, h);
      ctx.fillStyle = "#66737e";
      ctx.font = fs + "px Cascadia Mono, Consolas";
      ctx.textBaseline = "top";

      if (state.sim.path) {
        drawPathChart(w, h, fs);
        return;
      }
      if (state.sim.kernel) {
        drawKernelChart(w, h, fs);
        return;
      }

      ctx.fillStyle = "#52606b";
      ctx.textAlign = "center";
      ctx.fillText("MODEL VIEW · NO CONTROL STREAM", w / 2, h / 2 - fs / 2);
      ctx.textAlign = "left";
      ctx.fillStyle = "#66737e";
      ctx.fillText("GLTF NODES / STATIC PREVIEW", Math.round(10 * dpr), Math.round(8 * dpr));
    }

    function drawPathChart(w, h, fs) {
      const p = state.sim.path;
      const t = Math.max(0, Math.min(1, p.t || 0));
      ctx.fillStyle = "#66737e";
      ctx.fillText("KERNEL LINE PATH · s / t", Math.round(10 * (fs / 12)), Math.round(8 * (fs / 12)));
      ctx.fillStyle = "#1b242b";
      ctx.fillRect(w * 0.10, h / 2 - 4, w * 0.80, 8);
      ctx.fillStyle = p.state === 3 ? "#d95f5f" : (p.state === 2 ? "#6fbf73" : "#d79a52");
      ctx.fillRect(w * 0.10, h / 2 - 4, w * 0.80 * t, 8);
    }

    function drawKernelChart(w, h, fs) {
      const colors = ["#5b9dff", "#d79a52", "#9b8ad6", "#6fbf73", "#d16ab5", "#d95f5f", "#4fc3c8", "#b6a04f"];
      const data = state.sim.qHist;
      const n = data.length ? data[data.length - 1].length : 0;
      if (data.length > 1 && n) {
        for (let j = 0; j < n; j++) {
          ctx.strokeStyle = colors[j % colors.length];
          ctx.lineWidth = Math.max(1.2, 1.4 * (fs / 12));
          ctx.beginPath();
          data.forEach(function (q, i) {
            const deg = F.math.clamp(q[j] * F.math.R2D, -180, 180);
            const x = i / (data.length - 1) * w;
            const y = h - (deg + 180) / 360 * h;
            if (i) ctx.lineTo(x, y);
            else ctx.moveTo(x, y);
          });
          ctx.stroke();
        }
      }
      ctx.fillStyle = "#66737e";
      ctx.fillText(state.sim.manual ? "KERNEL q / MANUAL HOLD · DEG" : "KERNEL q / AUTO · DEG", Math.round(10 * (fs / 12)), Math.round(8 * (fs / 12)));
      for (let j = 0; j < n; j++) {
        ctx.fillStyle = colors[j % colors.length];
        ctx.fillText("J" + (j + 1), Math.round((180 + j * 18) * (fs / 12)), Math.round(8 * (fs / 12)));
      }
    }

    function update(now) {
      if (now - state.sim.lastUi < 80) return;
      state.sim.lastUi = now;

      const kernel = !!state.sim.kernel;
      const path = state.sim.path;
      if (path) {
        label("mLoop", "Loop");
        label("mRms", "Arc s");
        label("mPeak", "Param t");
        label("mTorque", "Point");
        label("mLoss", "Length");
        label("mTime", "State");
        F.dom.$("mLoop").textContent = "0x05";
        F.dom.$("mMode").textContent = ["idle", "run", "done", "fault"][path.state] || "?";
        F.dom.$("mRms").textContent = F.math.fmt(path.s, 3) + " m";
        F.dom.$("mPeak").textContent = F.math.fmt(path.t, 3);
        F.dom.$("mTorque").textContent = F.math.fmt(path.x, 3) + " / " + F.math.fmt(path.z, 3);
        F.dom.$("mLoss").textContent = F.math.fmt(path.length, 3) + " m";
        F.dom.$("mTime").textContent = path.fault ? "fault " + path.fault : "none";
        F.dom.$("mRms").nextElementSibling.textContent = "kernel arc length";
        F.dom.$("mPeak").nextElementSibling.textContent = "normalized arc";
        F.dom.$("mTorque").nextElementSibling.textContent = "path point x / z";
        F.dom.$("mLoss").nextElementSibling.textContent = "line segment";
        F.dom.$("mTime").nextElementSibling.textContent = "fault code";
        drawChart();
        onMetrics({ kernel: true, path: F.model.clone(path), pose: state.doc.pose ? F.model.clone(state.doc.pose) : null });
        return;
      }

      const dockMeta = F.dom.$("dockMeta");
      if (dockMeta) {
        dockMeta.textContent = kernel
          ? "KERNEL 0x03 · " + (state.sim.kernelStatus && state.sim.kernelStatus.mode === 1 ? "manual hold" : "auto")
          : "glTF 2.0 · " + (state.bridge.online ? "bridge online · wait 0x03" : "model view");
      }

      if (kernel) {
        const st = state.sim.kernelStatus || {};
        label("mLoop", "Kernel link");
        label("mRms", "Err");
        label("mPeak", "Manip");
        label("mTorque", "Sigma");
        label("mLoss", "Null");
        label("mTime", "Limit");
        F.dom.$("mLoop").textContent = "0x03";
        F.dom.$("mMode").textContent = st.mode === 1 ? "manual hold" : "auto fallback";
        F.dom.$("mRms").textContent = st.errMm == null ? "N/A" : F.math.fmt(st.errMm, 2) + " mm";
        F.dom.$("mPeak").textContent = st.wManip == null ? "N/A" : F.math.fmt(st.wManip, 4);
        F.dom.$("mTorque").textContent = st.sigmaMin == null ? "N/A" : F.math.fmt(st.sigmaMin, 3) + " / " + F.math.fmt(st.sigmaMax, 3);
        F.dom.$("mLoss").textContent = st.nullShare == null ? "N/A" : F.math.fmt(st.nullShare * 100, 1) + "%";
        F.dom.$("mTime").textContent = st.limitMargin == null ? "N/A" : F.math.fmt(st.limitMargin, 1) + "°";
        F.dom.$("mRms").nextElementSibling.textContent = "end effector";
        F.dom.$("mPeak").nextElementSibling.textContent = "manipulability";
        F.dom.$("mTorque").nextElementSibling.textContent = "sigma min / max";
        F.dom.$("mLoss").nextElementSibling.textContent = "null-space share";
        F.dom.$("mTime").nextElementSibling.textContent = "limit margin";
        drawChart();
        onMetrics({
          kernel: true,
          manual: !!state.sim.manual,
          q: state.sim.q.slice(),
          dq: (st.dq || []).slice(),
          qTarget: (st.qTarget || []).slice(),
          target: (st.target || [0, 0]).slice(),
          errMm: st.errMm,
          wManip: st.wManip,
          sigmaMin: st.sigmaMin,
          sigmaMax: st.sigmaMax,
          nullShare: st.nullShare,
          limitMargin: st.limitMargin,
          mode: st.mode
        });
        return;
      }

      const doc = state.doc;
      const meta = doc.meta || {};
      const nodes = doc.source && doc.source.nodes || doc.segments.length;
      const joints = doc.segments.filter(function (s) { return s.actuated; }).length;
      label("mLoop", "Kernel link");
      label("mRms", "Nodes");
      label("mPeak", "Joints");
      label("mTorque", "Triangles");
      label("mLoss", "Meshes");
      label("mTime", "Kind");
      F.dom.$("mLoop").textContent = state.bridge.online ? "BRIDGE WAIT" : "OFFLINE";
      F.dom.$("mMode").textContent = state.bridge.online ? "kernel did not send 0x03" : "page does not run control";
      F.dom.$("mRms").textContent = String(nodes);
      F.dom.$("mPeak").textContent = String(joints);
      F.dom.$("mTorque").textContent = String(meta.triangleCount || 0);
      F.dom.$("mLoss").textContent = String(doc.source && doc.source.meshes || 0);
      F.dom.$("mTime").textContent = doc.modelKind || (F.model.isImported(doc) ? "generic" : "arm");
      F.dom.$("mRms").nextElementSibling.textContent = "glTF scene";
      F.dom.$("mPeak").nextElementSibling.textContent = "semantic joints";
      F.dom.$("mTorque").nextElementSibling.textContent = "mesh faces";
      F.dom.$("mLoss").nextElementSibling.textContent = "glTF mesh";
      F.dom.$("mTime").nextElementSibling.textContent = "asset kind";

      drawChart();
      onMetrics({
        kernel: false,
        manual: false,
        nodes: nodes,
        joints: joints,
        triangles: meta.triangleCount || 0,
        meshes: doc.source && doc.source.meshes || 0,
        kind: doc.modelKind || "generic"
      });
    }

    return { update: update, resize: resize, drawChart: drawChart };
  }

  F.ui.createTelemetryPanel = createTelemetryPanel;
})(window.AEVUM.FORGE);
