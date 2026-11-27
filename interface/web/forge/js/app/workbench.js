/* Workbench shell: viewport input, view controls, panel/dock chrome. */
(function (F) {
  "use strict";

  const M = F.math;
  const $ = F.dom.$;

  F.app = F.app || {};
  F.app.createWorkbench = function (opts) {
    const state = opts.state;
    const canvas = opts.canvas;
    const renderer = opts.renderer;
    const labels = opts.labels;
    const doc = function () { return state.doc; };
    const onSelect = opts.onSelect || function () {};
    const onImport = opts.onImport || function () {};
    const app = $("workbench");
    const left = $("leftPanel");
    const right = $("rightPanel");
    const dock = $("dock");
    const dockBtn = $("btnDock");

    let drag = null;

    function sync() {
      const ortho = !!state.cam.ortho;
      const grid = !!renderer.options.grid;
      const clay = !!renderer.options.clay;
      const orthoBtn = $("btnOrtho");
      const gridBtn = $("btnGrid");
      const clayBtn = $("btnClay");
      const camBtn = $("btnCameraMode");
      const camPicker = $("cameraPicker");
      if (orthoBtn) orthoBtn.classList.toggle("active", ortho);
      if (gridBtn) gridBtn.classList.toggle("active", grid);
      if (clayBtn) clayBtn.classList.toggle("active", clay);
      if (camPicker && camBtn) {
        const isCar = !!(state.doc && state.doc.modelKind === "vehicle");
        camPicker.hidden = !isCar;
        camBtn.value = state.cam.mode;
      }
    }

    function toggleCamera() {
      if (!state.doc || state.doc.modelKind !== "vehicle") return;
      const next = state.cam.mode === "orbit" ? "fixed" :
        (state.cam.mode === "fixed" ? "follow" :
        (state.cam.mode === "follow" ? "first" : "orbit"));
      F.render.cameraSetMode(state.cam, next, state.doc);
      if (next === "orbit") F.render.fitCamera(state.doc, state.cam, true);
      sync();
    }

    function frameSelected() {
      const selected = state.selected;
      if (selected.kind !== "joint") {
        F.render.fitCamera(doc(), state.cam);
        return;
      }
      const scene = F.render.buildScene(doc());
      const link = scene.links[selected.index];
      if (!link) return;
      let center = M.translation(link.model);
      let span = link.seg.length || 0.1;
      if (link.mesh && link.mesh.bounds) {
        const b = link.mesh.bounds;
        if (b.center) center = M.transformPoint(link.model, b.center);
        if (b.size) span = Math.max(b.size[0], b.size[1], b.size[2], 0.05);
      } else {
        span = Math.max(link.seg.length || 0.1, link.seg.width || 0.06, link.seg.depth || 0.06);
      }
      F.render.cameraFrameAt(state.cam, center, span * 0.55);
    }

    function setView(kind) {
      F.render.setCameraView(state.cam, kind, doc());
    }

    function setHidden(panel, collapsed) {
      if (!panel) return;
      if (collapsed && panel.contains(document.activeElement)) document.activeElement.blur();
      panel.setAttribute("aria-hidden", collapsed ? "true" : "false");
      if (collapsed) panel.setAttribute("inert", "");
      else panel.removeAttribute("inert");
    }

    function toggleLeft(force) {
      const collapsed = force === undefined ? !app.classList.contains("left-collapsed") : force;
      app.classList.toggle("left-collapsed", collapsed);
      const btn = $("btnToggleLeft");
      if (btn) btn.textContent = collapsed ? "›" : "‹";
      setHidden(left, collapsed);
    }

    function toggleRight(force) {
      const collapsed = force === undefined ? !app.classList.contains("right-collapsed") : force;
      app.classList.toggle("right-collapsed", collapsed);
      const btn = $("btnToggleRight");
      if (btn) btn.textContent = collapsed ? "‹" : "›";
      setHidden(right, collapsed);
    }

    function toggleDock() {
      const expanded = !dock.classList.contains("expanded");
      dock.classList.toggle("expanded", expanded);
      if (dockBtn) dockBtn.textContent = expanded ? "Collapse" : "Expand";
      window.dispatchEvent(new Event("resize"));
    }

    function pick(clientX, clientY) {
      const rect = canvas.getBoundingClientRect();
      const x = clientX - rect.left;
      const y = clientY - rect.top;
      let best = null;
      let bestD = 22;
      labels.joints().forEach(function (j) {
        const d = Math.hypot(j.x - x, j.y - y);
        if (d < bestD) { bestD = d; best = j; }
      });
      if (best) {
        onSelect("joint", best.index);
        return;
      }
      bestD = 26;
      labels.parts().forEach(function (p) {
        const d = Math.hypot(p.x - x, p.y - y);
        if (d < bestD) { bestD = d; best = p; }
      });
      if (best) onSelect("joint", best.index);
      else onSelect("base", -1);
    }

    canvas.addEventListener("contextmenu", function (e) { e.preventDefault(); });
    canvas.addEventListener("pointerdown", function (e) {
      if (e.button !== 0 && e.button !== 1 && e.button !== 2) return;
      drag = { x: e.clientX, y: e.clientY, button: e.button, shift: e.shiftKey, moved: false };
      canvas.setPointerCapture(e.pointerId);
      canvas.focus();
    });
    canvas.addEventListener("pointermove", function (e) {
      if (!drag) return;
      const dx = e.clientX - drag.x;
      const dy = e.clientY - drag.y;
      if (Math.abs(dx) + Math.abs(dy) > 3) drag.moved = true;
      drag.x = e.clientX;
      drag.y = e.clientY;
      if (drag.button === 0 && !e.shiftKey) {
        state.cam.goalYaw -= dx * 0.0075;
        state.cam.goalPitch = M.clamp(state.cam.goalPitch + dy * 0.0065, -1.42, 1.42);
      } else {
        F.render.panCamera(state.cam, dx, dy, canvas.clientHeight);
      }
    });
    function endDrag(e) {
      if (!drag) return;
      const wasDrag = drag.moved;
      drag = null;
      if (canvas.hasPointerCapture(e.pointerId)) canvas.releasePointerCapture(e.pointerId);
      if (!wasDrag && e.button === 0) pick(e.clientX, e.clientY);
    }
    canvas.addEventListener("pointerup", endDrag);
    canvas.addEventListener("pointercancel", function (e) {
      drag = null;
      if (canvas.hasPointerCapture(e.pointerId)) canvas.releasePointerCapture(e.pointerId);
    });
    canvas.addEventListener("wheel", function (e) {
      e.preventDefault();
      state.cam.goalDist = M.clamp(state.cam.goalDist * Math.exp(e.deltaY * 0.0011), 0.35, 30);
    }, { passive: false });
    canvas.addEventListener("dblclick", function () { frameSelected(); });
    canvas.tabIndex = 0;

    if ($("btnFrame")) $("btnFrame").onclick = frameSelected;
    if ($("btnOrtho")) $("btnOrtho").onclick = function () {
      state.cam.ortho = !state.cam.ortho;
      sync();
    };
    if ($("btnGrid")) $("btnGrid").onclick = function () {
      renderer.setOptions({ grid: !renderer.options.grid });
      sync();
    };
    if ($("btnClay")) $("btnClay").onclick = function () {
      renderer.setOptions({ clay: !renderer.options.clay });
      sync();
    };

    document.querySelectorAll("#viewCube [data-view]").forEach(function (b) {
      b.onclick = function () { setView(b.dataset.view); };
    });

    if ($("btnToggleLeft")) $("btnToggleLeft").onclick = function () { toggleLeft(true); };
    if ($("btnToggleRight")) $("btnToggleRight").onclick = function () { toggleRight(true); };
    if ($("btnShowLeft")) $("btnShowLeft").onclick = function () { toggleLeft(false); };
    if ($("btnShowRight")) $("btnShowRight").onclick = function () { toggleRight(false); };
    if (dockBtn) dockBtn.onclick = toggleDock;

    if ($("btnImportTop")) $("btnImportTop").onclick = onImport;
    if ($("btnCameraMode")) {
      $("btnCameraMode").onchange = function (e) {
        const mode = e.target.value;
        F.render.cameraSetMode(state.cam, mode, state.doc);
        if (mode === "orbit") F.render.fitCamera(state.doc, state.cam, true);
        sync();
      };
    }

    window.addEventListener("keydown", function (e) {
      const tag = (e.target && e.target.tagName || "").toLowerCase();
      if (tag === "input" || tag === "textarea" || tag === "select") return;
      if (e.key === "v" || e.key === "V") { e.preventDefault(); toggleCamera(); }
      else if (e.key === "f" || e.key === "F") { e.preventDefault(); frameSelected(); }
      else if (e.key === "g" || e.key === "G") { e.preventDefault(); renderer.setOptions({ grid: !renderer.options.grid }); sync(); }
      else if (e.key === "o" || e.key === "O") { e.preventDefault(); state.cam.ortho = !state.cam.ortho; sync(); }
      else if (e.key === "1") { e.preventDefault(); setView("front"); }
      else if (e.key === "3") { e.preventDefault(); setView("right"); }
      else if (e.key === "7") { e.preventDefault(); setView("top"); }
      else if (e.key === "0") { e.preventDefault(); setView("home"); }
    });

    sync();
    return { sync: sync, frame: frameSelected, setView: setView, toggleCamera: toggleCamera };
  };
})(window.AEVUM.FORGE);
