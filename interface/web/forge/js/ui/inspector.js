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
      if (doc.modelKind === "arm" || !F.model.isImported(doc)) return "3R ARM";
      return "GENERIC";
    }

    function kernelCompat(doc) {
      if (doc && doc.modelKind === "vehicle") {
        return { ok: true, vehicle: true, reason: "vehicle avatar · 3R control" };
      }
      return F.model.isImported(doc)
        ? F.model.kernelCompatibility(doc)
        : { ok: true, reason: "native 3R chain" };
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
      const compat = kernelCompat(doc);
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
        + row("COMPAT", compat.vehicle ? "3R AVATAR" : (compat.ok ? "COMPATIBLE" : "MODEL VIEW"))
        + '<div class="note">' + esc(compat.vehicle
          ? "车辆是 3R 基座上的外观：q 只从 0x03 读取，车身位姿由同一组 q 推导。"
          : (compat.ok ? "可以把 body descriptor 交给内核，q 只从 0x03 状态帧读取。" : compat.reason)) + '</div>'
        + '</div>'
        + bridgeGroup();
      container.innerHTML = html;
      wireBridgeGroup();
    }

    function row(label, value) {
      if (typeof value === "string" && value.indexOf("<input") !== -1) {
        return '<div class="row input-row"><span>' + label + '</span>'
          + '<div class="ctl">' + value + '</div></div>';
      }
      return '<div class="row"><span>' + label + '</span><output>' + value + '</output></div>';
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

    function kernelStatusMarkup() {
      if (!state.sim.kernel) return "";
      if (state.doc && state.doc.modelKind === "vehicle") {
        return '<div class="group"><div class="gtitle">Kernel status <span class="meta">0x03</span></div>'
          + row("LINK", "KERNEL ONLINE")
          + row("POSE STREAM", "0x05 PATH")
          + '<div class="note">车辆没有 2–8 段 body q；位姿、切向、曲率与轮角只从 0x05 路径帧读取。</div>'
          + '</div>';
      }
      return (
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
      );
    }

    function renderNode() {
      const doc = state.doc;
      const i = state.selected.index;
      const s = doc.segments[i];
      if (!s) return;
      const mesh = s.mesh || {};
      const tris = mesh.triangles || 0;
      const parent = s.parent >= 0 && doc.segments[s.parent];
      const compat = kernelCompat(doc);

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
          + row("ACTUAL ANGLE", '<span id="vActual">' + M.fmt((s.q || 0) * M.R2D, 1) + '°</span>')
          + row("LIMITS", M.fmt(s.min, 0) + "° / " + M.fmt(s.max, 0) + "°")
          + '<div class="note">' + (doc.modelKind === "vehicle"
            ? "车辆关节由 0x05 路径帧驱动；页面不提供车辆专用按钮。"
            : (state.sim.kernel && compat.ok
              ? "W / A / S / D 只改末端目标，q_target 由页面的平面雅可比映射下发，运动与显示角度仍由内核 0x03 回传。"
              : "未接内核：角度只读。页面不做本地 PD。")) + '</div>'
          + '</div>'
          + kernelStatusMarkup();
      } else {
        html += '<div class="group"><div class="gtitle">STATIC PART</div>'
          + row("DRIVEN", "NO")
          + row("PARENT", parent ? esc(parent.id) : "SCENE")
          + '<div class="note">这是车身、底盘、车轮外形或工具外壳。它随父节点运动，但页面不产生关节，也不接受目标。</div>'
          + '</div>';
      }

      html += bridgeGroup();
      container.innerHTML = html;
      wireBridgeGroup();
    }

    function updateRuntime() {
      const s = state.doc.segments[state.selected.index];
      if (!s) return;
      function set(id, text) {
        const el = F.dom.$(id);
        if (el) el.textContent = text;
      }
      set("vActual", M.fmt((s.q || 0) * M.R2D, 1) + "°");
      set("vTarget", M.fmt((s.target || 0) * M.R2D, 1) + "°");
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
