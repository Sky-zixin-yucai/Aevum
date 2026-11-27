/* 可见的内核链路调试：body / J / 0x03 三个方向都摊开，不藏在控制台。 */
(function (F) {
  "use strict";

  F.ui = F.ui || {};

  function deg(v) {
    return (Number(v) || 0) * 180 / Math.PI;
  }

  function list(values, digits) {
    if (!values || !values.length) return "--";
    return values.map(function (v) {
      const n = Number(v);
      return Number.isFinite(n) ? n.toFixed(digits == null ? 2 : digits) : "--";
    }).join(" / ");
  }

  F.ui.createDebugPanel = function (opts) {
    const state = opts.state;
    const box = opts.box;
    const nowEl = opts.now;
    if (!box) return { update: function () {} };
    let last = 0;

    function update() {
      const t = performance.now();
      if (t - last < 200) return;
      last = t;
      const control = state.control || state.doc;
      const chain = F.model.controlSegments(control);
      const body = state.sim.lastBody;
      const cmd = state.sim.lastCommand;
      const st = state.sim.lastStatus;
      const values = st && st.values || {};
      const age = st && st.at ? Math.round((Date.now() - st.at) / 1000 * 10) / 10 : null;
      const actual = chain.map(function (s) { return deg(s.q); });
      const target = chain.map(function (s) { return deg(s.target); });
      const lines = [];

      lines.push("BRIDGE  " + (state.bridge.online ? "ONLINE" : "OFFLINE") + "  " + state.bridge.url);
      lines.push("MODEL   " + (state.doc.modelKind || (F.model.isImported(state.doc) ? "generic" : "arm")) + " · " +
        (state.doc.source && state.doc.source.file || "native") + " · " +
        chain.length + " control joints");
      lines.push("BODY→K  " + (body ? "n=" + body.n + "  L=" + list(body.lengths, 3) + " m" : "not sent"));
      if (body) lines.push("        qHome=" + list(body.qHome, 2) + " rad");
      lines.push("PAGE→K  " + (cmd ? "J q_target=" + list(cmd.q, 3) + " rad · " +
        Math.round((Date.now() - cmd.at) / 1000 * 10) / 10 + " s ago" : "no J yet"));
      lines.push("K→PAGE  " + (st ? "0x03 n=" + (values.n || st.n || "?") +
        " frame=" + (state.sim.statusCount || "?") +
        " mode=" + (values.mode === 1 ? "MANUAL" : "AUTO") +
        " · " + (age == null ? "?" : age + " s ago") : "no 0x03 yet"));
      if (st) {
        lines.push("        q_target=" + list((st.qTarget || []).map(deg), 2) + "°");
        lines.push("        q_actual=" + list((st.q || []).map(deg), 2) + "°");
        if (Number.isFinite(Number(values.tx)) && Number.isFinite(Number(values.ty))) {
          lines.push("        target=" + (Number(values.tx) / 65536).toFixed(3) + " / " +
            (Number(values.ty) / 65536).toFixed(3) + " m");
        }
      }
      lines.push("FORGE   q=" + list(actual, 1) + "°");
      lines.push("        tgt=" + list(target, 1) + "°");
      lines.push("NOTE    QEMU VGA 与 Forge 都读同一个内核 q；页面不画另一套运动。");

      box.textContent = lines.join("\n");
      if (nowEl) nowEl.textContent = "0x03 · " + (state.sim.statusCount || "--");
    }

    return { update: update };
  };
})(window.AEVUM.FORGE);
