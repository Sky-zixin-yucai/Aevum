/* Mode pill: describes where control comes from. It never starts or stops a controller. */
(function (F) {
  "use strict";

  function createStatus(opts) {
    const state = opts.state;
    const modePill = F.dom.$("modePill");
    const kernelPill = F.dom.$("kernelPill");

    function sync() {
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
