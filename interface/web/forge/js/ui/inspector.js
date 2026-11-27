/* Right-side Inspector: read-only model facts, plus kernel-owned joint targets. */
(function (F) {
  "use strict";

  const M = F.math;

  function esc(text) {
    return String(text == null ? "" : text).replace(/[&<>"]/g, function (c) {
      return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c];
    });
  }

  function createInspector(opts) {
    const state = opts.state;
    const container = opts.container;

    function kindName(doc) {
      if (doc.modelKind === "vehicle") return "VEHICLE";
      if (doc.modelKind === "arm") return "3R ARM";
      return "GENERIC";
    }

    function render() {
      if (state.selected.kind === "base") renderScene();
      else renderNode();
    }

    function renderScene() {
      const doc = state.doc;
      const meta = doc.meta || {};
      const source = doc.source || {};
      const aevum = doc.aevum || {};
      const joints = doc.segments.filter(function (s) { return s.actuated; }).length;
      const compat = F.model.isImported(doc) ? F.model.kernelCompatibility(doc) : { ok: true, reason: "native 3R chain" };
      let html = '<div class="group"><div class="gtitle">MODEL / ' + esc(kindName(doc)) + '</div>'
        + row("NAME", esc(doc.name || "-"))
        + row("NODES", doc.segments.length)
        + row("JOINTS", joints)
        + row("TRIANGLES", meta.triangleCount || 0)
        + row("MESHES", source.meshes || 0)
        + row("SOURCE", esc(source.file || "-"))
        + row("FORMAT", esc(source.format || "glTF 2.0"))
        + '</div>'
        + '<details class="group fold" open><summary><span class="chev">›</span>Semantics <span class="meta">asset.extras.aevum</span></summary>'
        + row("SCHEMA", esc(aevum.schema || "generic"))
        + row("KIND", esc(aevum.kind || kindName(doc)))
        + row("CONTROL", esc(aevum.control && (aevum.control.hz || aevum.control.loop_hz) ? (aevum.control.hz || aevum.control.loop_hz) + " Hz" : "-"))
        + row("PLACEHOLDER", aevum.placeholder ? "true" : "false")
        + '<div class="note">节点是几何；只有 joints[].target 指向的节点被标为 actuated。页面不积分、不跑 PD。</div>'
        + '</details>'
        + '<div class="group"><div class="gtitle">KERNEL HANDOFF</div>'
        + row("COMPAT", compat.ok ? "COMPATIBLE" : "MODEL VIEW")
        + '<div class="note">' + esc(compat.ok ? "可以把 body descriptor 交给内核，q 只从 0x03 状态帧读取。" : compat.reason) + '</div>'
        + '</div>'
        + documentActions()
        + bridgeGroup()
        + pathGroup();
      container.innerHTML = html;
      wireDocumentButtons();
      wireBridgeGroup();
      wirePathGroup();
    }

    function row(label, value) {
      if (typeof value === "string" && value.indexOf("<input") !== -1) {
        return '<div class="row input-row"><span>' + label + '</span>'
          + '<div class="ctl">' + value + '</div></div>';
      }
      return '<div class="row"><span>' + label + '</span><output>' + value + '</output></div>';
    }

    function documentActions() {
      return '<details class="group fold"><summary><span class="chev">›</span>Document <span class="meta">glTF 2.0</span></summary>'
        + '<div class="actions"><button class="amber" id="btnImport">Import glTF</button></div>'
        + '<div class="note">源模型只接受 .gltf / .glb。导入后节点按 role 分为 part 与 joint，静态节点不接受运动目标。</div></details>';
    }

    function bridgeGroup() {
      return '<details class="group fold"><summary><span class="chev">›</span>Kernel bridge <span class="meta">websocket</span></summary>'
        + '<div class="bridge"><div class="status ' + (state.bridge.online ? "on" : "") + '" id="bridgeStatus">'
        + (state.bridge.online ? "CONNECTED" : "DISCONNECTED") + '</div>'
        + '<input id="bridgeUrl" type="text" value="' + esc(state.bridge.url) + '">'
        + '<button style="width:100%" id="btnBridge">' + (state.bridge.online ? "Disconnect" : "Connect") + '</button>'
        + '<div class="note">桥接只转发命令与 0x03 状态；页面不伪造 q，不本地仿真。</div>'
        + '</div></details>';
    }

    function wireBridgeGroup() {
      const btn = F.dom.$("btnBridge");
      if (btn) btn.onclick = function () { opts.onToggleBridge(F.dom.$("bridgeUrl").value.trim()); };
    }

    /* 车辆只暴露几何参数实例；路径生成、停止和超时都在内核。 */
    function pathGroup() {
      if (!state.doc || state.doc.modelKind !== "vehicle") return "";
      const p = state.sim.path;
      const isArc = p && p.kind === 2;
      const lineState = p && !isArc ? (["IDLE", "RUN", "DONE", "FAULT"][p.state] || "?") : "IDLE";
      const arcState = isArc ? (["IDLE", "RUN", "DONE", "FAULT"][p.state] || "?") : "IDLE";
      return '<details class="group fold" id="pathGroup" open>'
        + '<summary><span class="chev">›</span>Kernel LINE <span class="meta">0x05</span></summary>'
        + row("X0 / Z0", '<input id="lineX0" type="number" step="0.01" value="0">'
          + '<input id="lineZ0" type="number" step="0.01" value="0">')
        + row("X1 / Z1", '<input id="lineX1" type="number" step="0.01" value="0.40">'
          + '<input id="lineZ1" type="number" step="0.01" value="0">')
        + row("SPEED m/s", '<input id="lineSpeed" type="number" step="0.01" value="0.20">')
        + '<div class="status" id="lineState">' + esc(lineState) + '</div>'
        + '<button style="width:100%" id="btnLine"' + (state.bridge.online ? "" : " disabled") + '>Run LINE</button>'
        + '<div class="note">只下发端点与速度；内核按 100 Hz 推进 s，页面只呈现路径点和轮子滚角，不本地积分。</div>'
        + '</details>'
        + '<details class="group fold" id="arcGroup" open>'
        + '<summary><span class="chev">›</span>Kernel ARC <span class="meta">0x05</span></summary>'
        + row("CENTER X / Z", '<input id="arcCx" type="number" step="0.01" value="0">'
          + '<input id="arcCz" type="number" step="0.01" value="-0.50">')
        + row("RADIUS m", '<input id="arcR" type="number" step="0.01" value="0.50">')
        + row("START / SWEEP °", '<input id="arcA0" type="number" step="1" value="90">'
          + '<input id="arcSweep" type="number" step="1" value="-90">')
        + row("SPEED m/s", '<input id="arcSpeed" type="number" step="0.01" value="0.15">')
        + '<div class="status" id="arcState">' + esc(arcState) + '</div>'
        + '<button style="width:100%" id="btnArc"' + (state.bridge.online ? "" : " disabled") + '>Run ARC</button>'
        + '<div class="note">圆心、半径、起始角和扫角都交给内核；前轮转角由内核回传的曲率换算，页面不本地积分。</div>'
        + '</details>';
    }

    function wirePathGroup() {
      const num = function (id, fallback) {
        const el = F.dom.$(id);
        const v = el ? Number(el.value) : NaN;
        return isFinite(v) ? v : fallback;
      };
      const line = F.dom.$("btnLine");
      if (line) line.onclick = function () {
        if (!opts.onPathLine) return;
        opts.onPathLine({
          kind: 1,
          x0: num("lineX0", 0),
          z0: num("lineZ0", 0),
          x1: num("lineX1", 0.4),
          z1: num("lineZ1", 0),
          speed: num("lineSpeed", 0.2),
          tol: 0.01,
          timeout: 3000
        });
      };
      const arc = F.dom.$("btnArc");
      if (arc) arc.onclick = function () {
        if (!opts.onPathArc) return;
        opts.onPathArc({
          kind: 2,
          cx: num("arcCx", 0),
          cz: num("arcCz", -0.5),
          r: num("arcR", 0.5),
          a0: num("arcA0", 90) * M.D2R,
          sweep: num("arcSweep", -90) * M.D2R,
          speed: num("arcSpeed", 0.15),
          tol: 0.01,
          timeout: 5000
        });
      };
    }

    function kernelStatusMarkup() {
      return state.sim.kernel ? (
        '<details class="group fold" id="kernelStatusGroup" open><summary><span class="chev">›</span>Kernel status <span class="meta">0x03</span></summary>'
        + row("MODE", '<span id="ksMode">AUTO</span>')
        + row("END ERROR", '<span id="ksErr">-- mm</span>')
        + row("LIMIT MARGIN", '<span id="ksMargin">--</span>')
        + '<details class="subfold"><summary>Advanced kinematics</summary>'
        + row("MANIPULABILITY", '<span id="ksW">--</span>')
        + row("SIGMA MIN / MAX", '<span id="ksSigma">--</span>')
        + row("NULL SHARE", '<span id="ksNull">--</span>')
        + row("MAX |dq|", '<span id="ksDq">--</span>')
        + row("TARGET X / Y", '<span id="ksXY">--</span>')
        + '</details>'
        + '<div class="note">q 来自内核状态帧。手动保持 5 秒无新命令回 AUTO。</div></details>'
      ) : "";
    }

    function renderNode() {
      const doc = state.doc;
      const i = state.selected.index;
      const s = doc.segments[i];
      if (!s) return;
      const mesh = s.mesh || {};
      const tris = mesh.triangles || 0;
      const parent = s.parent >= 0 && doc.segments[s.parent];
      const compat = F.model.isImported(doc) ? F.model.kernelCompatibility(doc) : { ok: true, reason: "native 3R chain" };

      let html = '<div class="group"><div class="gtitle">' + (s.actuated ? "JOINT" : "PART") + ' / ' + esc(s.semanticId || s.id) + '</div>'
        + row("NAME", esc(s.name || s.id))
        + row("NODE", esc(s.id))
        + row("PARENT", parent ? esc(parent.id) : "SCENE")
        + row("ROLE", esc(s.role || "part"))
        + row("MESH", tris ? tris + " tris" : "none")
        + (s.actuated ? row("JOINT TYPE", esc(s.jointType || "revolute")) + row("CHANNEL", esc(s.channel || "-")) : "")
        + '</div>';

      if (s.actuated) {
        html += '<div class="group"><div class="gtitle">KERNEL-OWNED MOTION</div>'
          + row("TARGET ANGLE", '<span id="vTarget">' + M.fmt((s.target || 0) * M.R2D, 1) + '°</span>')
          + '<input id="inTarget" type="range" min="' + s.min + '" max="' + s.max + '" step="0.1" value="' + M.fmt((s.target || 0) * M.R2D, 1) + '"' + (state.sim.kernel && compat.ok ? "" : " disabled") + '>'
          + row("ACTUAL ANGLE", '<span id="vActual">' + M.fmt((s.q || 0) * M.R2D, 1) + '°</span>')
          + row("LIMITS", M.fmt(s.min, 0) + "° / " + M.fmt(s.max, 0) + "°")
          + '<div class="note">' + (state.sim.kernel && compat.ok
            ? "拖动只下发 q_target；显示角度来自内核 0x03。"
            : "未接内核：角度只读。页面不做本地 PD。") + '</div>'
          + '</div>'
          + kernelStatusMarkup();
        bind("inTarget", function (v) {
          if (!state.sim.kernel || !compat.ok) return;
          s.target = +v * M.D2R;
          const out = F.dom.$("vTarget");
          if (out) out.textContent = M.fmt(+v, 1) + "°";
          if (opts.onKernelTarget) opts.onKernelTarget();
        });
      } else {
        html += '<div class="group"><div class="gtitle">STATIC PART</div>'
          + row("DRIVEN", "NO")
          + row("PARENT", parent ? esc(parent.id) : "SCENE")
          + '<div class="note">这是车身、底盘、车轮外形或工具外壳。它随父节点运动，但页面不产生关节，也不接受目标。</div>'
          + '</div>';
      }

      html += documentActions() + bridgeGroup();
      container.innerHTML = html;
      wireDocumentButtons();
      wireBridgeGroup();
    }

    function bind(id, handler) {
      const el = F.dom.$(id);
      if (!el) return;
      el.oninput = function (e) { handler(e.target.value); };
    }

    function wireDocumentButtons() {
      const imp = F.dom.$("btnImport");
      if (imp) imp.onclick = function () { if (opts.onImport) opts.onImport(); };
    }

    function updateRuntime() {
      const s = state.doc.segments[state.selected.index];
      if (!s) return;
      function set(id, text) {
        const el = F.dom.$(id);
        if (el) el.textContent = text;
      }
      set("vActual", M.fmt((s.q || 0) * M.R2D, 1) + "°");
      const slider = F.dom.$("inTarget");
      if (slider && document.activeElement !== slider) {
        slider.value = M.fmt((s.target || 0) * M.R2D, 1);
        set("vTarget", M.fmt((s.target || 0) * M.R2D, 1) + "°");
      }
      if (!state.sim.kernel) return;
      const st = state.sim.kernelStatus;
      if (!st) return;
      set("ksMode", st.mode === 1 ? "MANUAL HOLD" : "AUTO");
      set("ksErr", M.fmt(st.errMm, 2) + " mm");
      set("ksW", M.fmt(st.wManip, 4));
      set("ksSigma", M.fmt(st.sigmaMin, 4) + " / " + M.fmt(st.sigmaMax, 4));
      set("ksNull", M.fmt(st.nullShare * 100, 1) + " %");
      set("ksMargin", M.fmt(st.limitMargin, 1) + "°");
      const dq = st.dq && st.dq.length ? Math.max.apply(null, st.dq.map(function (v) { return Math.abs(v); })) : 0;
      set("ksDq", M.fmt(dq, 3) + " rad/s");
      set("ksXY", M.fmt(st.target[0], 3) + " / " + M.fmt(st.target[1], 3) + " m");
    }

    return { render: render, updateRuntime: updateRuntime };
  }

  F.ui.createInspector = createInspector;
})(window.AEVUM.FORGE);
