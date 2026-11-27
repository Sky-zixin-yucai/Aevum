/* RequestAnimationFrame loop. It renders and reads telemetry; it never runs a controller. */
(function (F) {
  "use strict";

  F.app = F.app || {};
  F.app.startFrameLoop = function (opts) {
    const state = opts.state;
    const renderer = opts.renderer;
    const telemetry = opts.telemetry;
    const inspector = opts.inspector;
    const tree = opts.tree;
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
      const scene = F.render.buildScene(state.doc);
      const vp = F.render.viewProjection(state.cam, renderer.canvas);
      renderer.draw(state.doc, scene, vp, state.selected, eye);

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
