/* API command implementation. Public signatures are declared in api.js only. */
(function (F) {
  "use strict";

  F.app = F.app || {};
  F.app.createApiHandlers = function (deps) {
    const state = deps.state;
    const controls = deps.controls;
    const inspector = deps.inspector;
    const replaceDocument = deps.replaceDocument;
    const saveDoc = deps.saveDoc;
    const bridgeControl = deps.bridgeControl;
    const demoMotion = deps.demoMotion;
    const toast = deps.toast;
    const M = F.math;

    function getState() {
      return {
        running: state.sim.running,
        sine: state.sim.sine,
        kernel: state.sim.kernel,
        manual: state.sim.manual,
        kernelStatus: state.sim.kernelStatus ? F.model.clone(state.sim.kernelStatus) : null,
        time: state.sim.time,
        q: state.sim.q.slice(),
        target: state.doc.segments.map(function (s) { return s.target; }),
        demo: {
          sweep: !!state.demo.sweep,
          grabbing: !!state.demo.grabbing,
          grip: Number(state.doc.tool && state.doc.tool.grip) || 0,
          held: !!(state.doc.prop && state.doc.prop.held)
        },
        selected: { kind: state.selected.kind, index: state.selected.index },
        bridge: { online: state.bridge.online, url: state.bridge.url, kernel: state.sim.kernel },
        document: F.model.clone(state.doc)
      };
    }

    function requireLocal(action) {
      if (state.sim.kernel) {
        throw new Error(action + " 不可用：当前是 kernel q 只读模式");
      }
    }

    return {
      getState: getState,
      getDocument: function () { return F.model.clone(state.doc); },
      setDocument: function (doc) { replaceDocument(F.model.clone(doc), true); },
      importModel: function (name, buffer) {
        const doc = F.model.importGltfBuffer(buffer, name);
        replaceDocument(doc, true);
        return {
          format: doc.format,
          segments: doc.segments.length,
          triangles: (doc.meta && doc.meta.triangleCount) || 0,
          kernel: F.model.kernelCompatibility(doc)
        };
      },
      setTarget: function (index, degrees) {
        const s = state.doc.segments[index];
        if (!s) throw new Error("joint index out of range");
        s.target = degrees * M.D2R;
        if (state.sim.kernel) bridgeControl.pulseManual();
        inspector.render();
      },
      setTargets: function (degrees) {
        degrees.forEach(function (v, i) {
          const s = state.doc.segments[i];
          if (s) s.target = v * M.D2R;
        });
        if (state.sim.kernel) bridgeControl.pulseManual();
        inspector.render();
      },
      setManual: function (on) {
        bridgeControl.setManual(on);
      },
      sweep: function (on) {
        if (!demoMotion) return false;
        if (on === false) demoMotion.stopSweep(true);
        else demoMotion.startSweep();
        controls.sync();
        return state.demo.sweep;
      },
      grab: function () {
        const ok = demoMotion ? demoMotion.grab() : false;
        controls.sync();
        return ok;
      },
      releaseGrip: function () {
        if (demoMotion) demoMotion.release();
        controls.sync();
        return !(state.doc.prop && state.doc.prop.held);
      },
      run: function () {
        if (state.sim.kernel) {
          bridgeControl.setManual(true);
          return;
        }
        state.sim.running = true;
        state.sim.last = performance.now();
        state.sim.acc = 0;
        controls.sync();
      },
      pause: function () {
        if (state.sim.kernel) {
          bridgeControl.setManual(false);
          return;
        }
        state.sim.running = false;
        controls.sync();
      },
      reset: function () {
        requireLocal("reset");
        if (demoMotion) demoMotion.reset();
        F.control.reset(state.doc, state.sim);
        controls.sync();
      },
      sine: function (on) {
        requireLocal("sine");
        state.sim.sine = on === undefined ? !state.sim.sine : !!on;
        if (state.sim.sine) state.sim.running = true;
        state.sim.last = performance.now();
        controls.sync();
      },
      getTelemetry: function () {
        if (state.sim.kernel) {
          const st = state.sim.kernelStatus || {};
          return {
            kernel: true,
            manual: !!state.sim.manual,
            q: state.sim.q.slice(),
            dq: (st.dq || [0, 0, 0]).slice(),
            qTarget: (st.qTarget || [0, 0, 0]).slice(),
            target: (st.target || [0, 0]).slice(),
            errMm: st.errMm == null ? null : st.errMm,
            wManip: st.wManip == null ? null : st.wManip,
            sigmaMin: st.sigmaMin == null ? null : st.sigmaMin,
            sigmaMax: st.sigmaMax == null ? null : st.sigmaMax,
            nullShare: st.nullShare == null ? null : st.nullShare,
            limitMargin: st.limitMargin == null ? null : st.limitMargin,
            mode: st.mode == null ? null : st.mode,
            rms: null,
            peak: null,
            torque: null,
            loss: 0
          };
        }
        return F.control.metrics(state.doc, state.sim);
      },
      exportJson: saveDoc,
      exportXml: function () { F.model.exportXml(state.doc, toast); },
      exportManifest: function () { F.model.exportManifest(state.doc, state.bridge, toast); },
      connect: bridgeControl.connect,
      disconnect: bridgeControl.disconnect
    };
  };
})(window.AEVUM.FORGE);
