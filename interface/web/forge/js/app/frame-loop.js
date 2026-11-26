/* RequestAnimationFrame loop. Owns time order, not scene or control formulas. */
(function (F) {
  "use strict";

  F.app = F.app || {};
  F.app.startFrameLoop = function (opts) {
    const state = opts.state;
    const renderer = opts.renderer;
    const telemetry = opts.telemetry;
    const bridge = opts.bridge;
    const inspector = opts.inspector;
    const tree = opts.tree;
    const demoMotion = opts.demoMotion;
    let treeLast = 0;
    let fpsFrames = 0;
    let fpsLast = 0;

    function frame(now) {
      if (document.hidden) {
        requestAnimationFrame(frame);
        return;
      }
      F.render.cameraStep(state.cam, now);
      const eye = F.render.cameraEye(state.cam);
      if (demoMotion) demoMotion.tick(now);
      const scene = F.render.buildScene(state.doc);
      const vp = F.render.viewProjection(state.cam, renderer.canvas);
      renderer.draw(state.doc, scene, vp, state.selected, eye);

      if (state.sim.running && !state.sim.kernel) {
        F.control.tick(state.doc, state.sim, now, function (packet) {
          F.bridge.send(bridge, packet);
        });
      }
      if (state.sim.kernel && (state.sim.manual || state.sim.demoDrive)) {
        F.bridge.send(bridge, {
          type: "joint",
          q: state.doc.segments.map(function (s) { return +s.target.toFixed(5); })
        });
      }
      telemetry.update(now);
      if (inspector) inspector.updateRuntime();
      if (tree && now - treeLast > 200) {
        treeLast = now;
        tree.updateRuntime();
      }

      fpsFrames++;
      if (now - fpsLast > 600) {
        const fps = Math.round(fpsFrames * 1000 / Math.max(1, now - fpsLast));
        const el = document.getElementById("statFps");
        if (el) el.textContent = fps + " FPS";
        fpsFrames = 0;
        fpsLast = now;
      }

      requestAnimationFrame(frame);
    }

    requestAnimationFrame(frame);
  };
})(window.AEVUM.FORGE);
