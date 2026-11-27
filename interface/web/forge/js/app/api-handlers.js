/* API command implementation. Public signatures are declared in api.js only. */
(function (F) {
  "use strict";

  F.app = F.app || {};
  F.app.createApiHandlers = function (deps) {
    const state = deps.state;
    const inspector = deps.inspector;
    const replaceDocument = deps.replaceDocument;
    const bridgeControl = deps.bridgeControl;
    const toast = deps.toast;
    const M = F.math;

    function requireKernel(action) {
      if (!state.sim.kernel) throw new Error(action + " 需要内核 0x03 接管；页面不做本地控制");
      const compat = F.model.kernelCompatibility(state.doc);
      if (!compat.ok) throw new Error(action + " 不可用：" + compat.reason);
    }

    function getState() {
      const doc = state.doc;
      return {
        viewOnly: true,
        modelKind: doc.modelKind || "generic",
        kernel: state.sim.kernel,
        manual: state.sim.manual,
        kernelStatus: state.sim.kernelStatus ? F.model.clone(state.sim.kernelStatus) : null,
        path: state.sim.path ? F.model.clone(state.sim.path) : null,
        q: state.sim.q.slice(),
        target: doc.segments.map(function (s) { return s.target; }),
        selected: { kind: state.selected.kind, index: state.selected.index },
        bridge: { online: state.bridge.online, url: state.bridge.url, kernel: state.sim.kernel },
        document: F.model.clone(doc)
      };
    }

    return {
      getState: getState,
      getDocument: function () { return F.model.clone(state.doc); },
      setDocument: function (doc) { replaceDocument(F.model.clone(doc), true); },
      importModel: function (name, buffer) {
        const doc = F.model.importGltfBuffer(buffer, name);
        replaceDocument(doc, true);
        return {
          format: "glTF 2.0",
          modelKind: doc.modelKind,
          nodes: doc.segments.length,
          triangles: (doc.meta && doc.meta.triangleCount) || 0,
          kernel: F.model.kernelCompatibility(doc)
        };
      },
      setTarget: function (index, degrees) {
        requireKernel("setTarget");
        const s = state.doc.segments[index];
        if (!s || !s.actuated) throw new Error("node is not an actuated joint");
        s.target = degrees * M.D2R;
        bridgeControl.pulseManual();
        inspector.render();
      },
      setTargets: function (degrees) {
        requireKernel("setTargets");
        degrees.forEach(function (v, i) {
          const s = state.doc.segments[i];
          if (s && s.actuated) s.target = v * M.D2R;
        });
        bridgeControl.pulseManual();
        inspector.render();
      },
      setManual: function (on) {
        requireKernel("setManual");
        bridgeControl.setManual(on);
      },
      getTelemetry: function () {
        if (state.sim.path) {
          return {
            kernel: true,
            path: F.model.clone(state.sim.path),
            pose: state.doc.pose ? F.model.clone(state.doc.pose) : null
          };
        }
        if (state.sim.kernel) {
          const st = state.sim.kernelStatus || {};
          return {
            kernel: true,
            manual: !!state.sim.manual,
            q: state.sim.q.slice(),
            dq: (st.dq || []).slice(),
            qTarget: (st.qTarget || []).slice(),
            target: (st.target || [0, 0]).slice(),
            errMm: st.errMm == null ? null : st.errMm,
            wManip: st.wManip == null ? null : st.wManip,
            sigmaMin: st.sigmaMin == null ? null : st.sigmaMin,
            sigmaMax: st.sigmaMax == null ? null : st.sigmaMax,
            nullShare: st.nullShare == null ? null : st.nullShare,
            limitMargin: st.limitMargin == null ? null : st.limitMargin,
            mode: st.mode == null ? null : st.mode
          };
        }
        const doc = state.doc;
        return {
          kernel: false,
          viewOnly: true,
          nodes: doc.segments.length,
          joints: doc.segments.filter(function (s) { return s.actuated; }).length,
          triangles: (doc.meta && doc.meta.triangleCount) || 0,
          meshes: doc.source && doc.source.meshes || 0,
          kind: doc.modelKind || "generic"
        };
      },
      connect: bridgeControl.connect,
      disconnect: bridgeControl.disconnect,
      runLine: function (params) {
        if (!state.bridge.online) throw new Error("runLine 需要 bridge 连接");
        return bridgeControl.sendPath(Object.assign({}, params || {}, { kind: 1 }));
      },
      runArc: function (params) {
        if (!state.bridge.online) throw new Error("runArc 需要 bridge 连接");
        return bridgeControl.sendPath(Object.assign({}, params || {}, { kind: 2 }));
      }
    };
  };
})(window.AEVUM.FORGE);
