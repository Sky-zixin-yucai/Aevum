/* Mode pill: describes where control comes from. It never starts or stops a controller. */
(function (F) {
  "use strict";

  function createStatus(opts) {
    const state = opts.state;
    const modePill = F.dom.$("modePill");
    const kernelPill = F.dom.$("kernelPill");

    function hud() {
      const title = F.dom.$("hudTitle");
      const sub = document.querySelector(".hud .sub");
      const doc = state.doc || {};
      const kind = doc.modelKind || (F.model.isImported(doc) ? "generic" : "arm");
      const name = kind === "vehicle" ? "TOY OFFROAD" : (kind === "arm" ? "3R ARM" : "IMPORTED");
      if (title) title.textContent = state.sim.kernel ? name + " / KERNEL LINK" : name + " / MODEL VIEW";
      if (!sub) return;
      if (state.sim.kernel) {
        sub.textContent = kind === "vehicle"
          ? "3R avatar · 0x03 q · W/S drive · A/D steer"
          : "0x03 q feedback · W/A/S/D target jog";
      } else if (state.bridge.online) {
        sub.textContent = "bridge online · waiting for 0x03";
      } else {
        sub.textContent = "glTF 2.0 · kernel offline · model view";
      }
    }

    function sync() {
      hud();
      if (!modePill) return;
      if (state.sim.kernel) {
        const manual = !!state.sim.kernelStatus && state.sim.kernelStatus.mode === 1;
        modePill.textContent = manual ? "KERNEL / MANUAL HOLD" : "KERNEL / AUTO";
        modePill.className = "pill on";
        return;
      }
      if (state.bridge.online) {
        modePill.textContent = "BRIDGE / WAIT 0x03";
        modePill.className = "pill on";
        return;
      }
      modePill.textContent = "MODEL VIEW";
      modePill.className = "pill";
    }

    function setKernel(mode) {
      if (!kernelPill) return;
      if (mode === "kernel") {
        kernelPill.textContent = "KERNEL LINK";
        kernelPill.className = "pill on";
      } else if (mode === "bridge") {
        kernelPill.textContent = "BRIDGE / WAIT q";
        kernelPill.className = "pill warn";
      } else if (mode === "model") {
        kernelPill.textContent = "MODEL ONLY";
        kernelPill.className = "pill warn";
      } else {
        kernelPill.textContent = "KERNEL OFFLINE";
        kernelPill.className = "pill warn";
      }
    }

    return { sync: sync, setKernel: setKernel };
  }

  F.ui = F.ui || {};
  F.ui.createStatus = createStatus;
})(window.AEVUM.FORGE);
