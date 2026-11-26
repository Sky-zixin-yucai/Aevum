/* Root composition: state, module wiring, document actions, boot. */
(function (F) {
  "use strict";

  const $ = F.dom.$;

  const state = {
    doc: F.model.defaultDoc(),
    selected: { kind: "joint", index: 0 },
    sim: null,
    demo: { sweep: false, sweepStart: 0, sweepBase: 0, grabbing: false, grabAt: 0, grabBy: 0 },
    cam: F.render.createCamera(),
    bridge: F.bridge.create("ws://127.0.0.1:8765")
  };
  state.sim = F.control.createSim(state.doc);

  const canvas = $("gl");
  let labels = null;
  let renderer = null;
  try {
    labels = F.render.createJointLabels($("labels"), canvas);
    renderer = F.render.createRenderer(canvas, labels);
  } catch (e) {
    const stage = document.querySelector(".stage");
    if (stage) {
      stage.innerHTML = '<div style="padding:30px;color:#e45b58;font:600 14px Segoe UI">WebGL 不可用。请用 Chrome / Edge 打开。</div>';
    }
    return;
  }

  let tree = null;
  let inspector = null;
  let controls = null;
  let telemetry = null;
  let workbench = null;
  let demoMotion = null;

  function setKernelPill(mode) {
    const pill = $("kernelPill");
    if (mode === "kernel") {
      pill.textContent = "KERNEL LINK";
      pill.className = "pill on";
    } else if (mode === "bridge") {
      pill.textContent = "BRIDGE / WAIT q";
      pill.className = "pill warn";
    } else {
      pill.textContent = "KERNEL OFFLINE";
      pill.className = "pill warn";
    }
  }

  function syncDocHeader() {
    const title = $("hudTitle");
    const sub = document.querySelector(".hud .sub");
    const docName = $("docName");
    const docMeta = $("docMeta");
    const imported = F.model.isImported(state.doc);
    const segs = state.doc.segments || [];
    const reach = segs.reduce(function (sum, seg) { return sum + (Number(seg.length) || 0); }, 0) +
      (state.doc.tool ? Number(state.doc.tool.length) || 0 : 0);
    const tris = (state.doc.meta && state.doc.meta.triangleCount) || 0;
    if (docName) docName.textContent = imported ? state.doc.name : "ARM" + segs.length + " / NATIVE";
    if (docMeta) {
      docMeta.textContent = imported
        ? segs.length + " links · " + tris + " tris · " + reach.toFixed(2) + " m"
        : segs.length + " joints · " + reach.toFixed(2) + " m reach · 100 Hz";
    }
    if (title) title.textContent = imported ? "IMPORTED / " + state.doc.name : "ARM" + segs.length + " / KINEMATIC BENCH";
    if (sub) sub.textContent = imported
      ? "glTF 2.0 · " + segs.length + " links · " + tris + " tris · joint-space PD"
      : "own renderer · " + segs.length + "R chain · 100 Hz control seam";
    const brandSub = document.querySelector(".brand small");
    if (brandSub) brandSub.textContent = "ROBOTICS WORKBENCH";
  }

  function replaceDocument(doc, notify) {
    state.doc = F.model.validate(doc);
    state.selected = { kind: "joint", index: 0 };
    state.sim = F.control.createSim(state.doc);
    state.sim.bodyPending = false;
    if (demoMotion) demoMotion.reset();
    labels.ensure(state.doc);
    if (F.model.isImported(state.doc)) F.render.fitCamera(state.doc, state.cam, true);
    else {
      Object.assign(state.cam, F.render.createCamera());
      F.render.fitCamera(state.doc, state.cam, true);
    }
    const preset = $("bodyPreset");
    if (preset) {
      preset.value = !F.model.isImported(state.doc) && state.doc.segments.length >= 2 && state.doc.segments.length <= 8
        ? String(state.doc.segments.length) : "custom";
    }
    if (state.bridge.online && bridgeControl) bridgeControl.sendBody();
    tree.render();
    inspector.render();
    controls.sync();
    telemetry.resize();
    if (F.model.isImported(state.doc) && !F.model.kernelCompatibility(state.doc).ok) {
      const pill = $("kernelPill");
      pill.textContent = "KERNEL LOCAL ONLY";
      pill.className = "pill warn";
    } else {
      setKernelPill(state.bridge.online ? "bridge" : false);
    }
    if (notify) F.ui.toast.show("BodyDocument 已加载");
    syncDocHeader();
  }

  function saveDoc() {
    F.model.exportJson(state.doc, F.ui.toast.show);
  }

  function loadDoc(file) {
    F.dom.readFile(file, function (err, text) {
      if (err) {
        F.ui.toast.show("加载失败: " + err.message);
        return;
      }
      try {
        replaceDocument(JSON.parse(text), true);
      } catch (e) {
        F.ui.toast.show("加载失败: " + e.message);
      }
    });
  }

  function loadModelFiles(fileList) {
    const files = Array.prototype.slice.call(fileList || []);
    if (!files.length) return;
    const main = files.find(function (f) { return /\.(glb|gltf)$/i.test(f.name); });
    if (!main) {
      F.ui.toast.show("请选择 .glb 或 .gltf 文件");
      return;
    }
    const loaded = [];
    let pending = files.length;
    files.forEach(function (file) {
      F.dom.readFileBuffer(file, function (err, buffer) {
        pending--;
        if (err) {
          F.ui.toast.show("读取失败: " + file.name + " / " + err.message);
          return;
        }
        loaded.push({ name: file.name, data: buffer });
        if (pending === 0) {
          try {
            const doc = F.model.importGltfFiles(loaded, main.name);
            replaceDocument(doc, false);
            F.ui.toast.show("已导入 " + doc.segments.length + " 个 link / " +
              ((doc.meta && doc.meta.triangleCount) || 0) + " 个三角形");
          } catch (e) {
            F.ui.toast.show("glTF 导入失败: " + e.message);
          }
        }
      });
    });
  }

  const bridgeControl = F.app.createBridgeControl({
    state: state,
    setKernelPill: setKernelPill,
    rerenderInspector: function () { if (inspector) inspector.render(); },
    syncControls: function () { if (controls) controls.sync(); },
    onKernelState: function () { if (tree) tree.render(); },
    updateKernelStatus: function () { if (inspector) inspector.updateRuntime(); },
    toast: F.ui.toast.show
  });

  demoMotion = F.control.createDemoMotion({
    state: state,
    bridgeControl: bridgeControl,
    syncControls: function () { if (controls) controls.sync(); },
    toast: F.ui.toast.show
  });

  tree = F.ui.createDocumentTree({
    state: state,
    tree: $("tree"),
    purpose: $("purpose"),
    selId: $("selId"),
    search: $("treeSearch"),
    onSelect: function () {
      if (inspector) inspector.render();
      if (workbench) workbench.sync();
    }
  });

  inspector = F.ui.createInspector({
    state: state,
    container: $("inspector"),
    onTreeChange: function () { tree.render(); },
    onSave: saveDoc,
    onLoad: function () { $("fileDoc").click(); },
    onImport: function () { $("fileModel").click(); },
    onExportXml: function () {
      if (F.model.isImported(state.doc)) {
        F.ui.toast.show("导入模型暂不导出 MuJoCo XML；请用 SAVE JSON 保存建模树");
        return;
      }
      F.model.exportXml(state.doc, F.ui.toast.show);
    },
    onExportManifest: function () { F.model.exportManifest(state.doc, state.bridge, F.ui.toast.show); },
    onToggleBridge: bridgeControl.toggle,
    onBodyChange: function () { if (state.bridge.online) bridgeControl.sendBody(); },
    onKernelTarget: bridgeControl.pulseManual
  });

  controls = F.ui.createControls({
    state: state,
    onRun: function () {
      if (state.sim.kernel) {
        bridgeControl.setManual(!state.sim.manual);
        controls.sync();
        return;
      }
      state.sim.running = !state.sim.running;
      state.sim.last = performance.now();
      state.sim.acc = 0;
    },
    onSine: function () {
      state.sim.sine = !state.sim.sine;
      state.sim.running = true;
      state.sim.last = performance.now();
      F.ui.toast.show(state.sim.sine ? "正弦激励已开启" : "已回到目标角");
    },
    onSweep: function () {
      if (demoMotion) demoMotion.toggleSweep();
    },
    onGrab: function () {
      if (demoMotion) demoMotion.grab();
    },
    onReset: function () {
      if (demoMotion) demoMotion.reset();
      F.control.reset(state.doc, state.sim);
      F.ui.toast.show("已复位到目标姿态");
    }
  });

  telemetry = F.ui.createTelemetryPanel({
    state: state,
    chart: $("chart"),
    onMetrics: function (metrics) {
      api._emit({
        type: "telemetry",
        rms: metrics.rms,
        peak: metrics.peak,
        torque: metrics.torque,
        time: state.sim.time,
        running: state.sim.running,
        sine: state.sim.sine,
        bridge: state.bridge.online
      });
    }
  });

  workbench = F.app.createWorkbench({
    state: state,
    canvas: canvas,
    renderer: renderer,
    labels: labels,
    onSelect: function (kind, index) { tree.select(kind, index); },
    onSave: saveDoc,
    onImport: function () { $("fileModel").click(); }
  });

  const handlers = F.app.createApiHandlers({
    state: state,
    controls: controls,
    inspector: inspector,
    replaceDocument: replaceDocument,
    saveDoc: saveDoc,
    bridgeControl: bridgeControl,
    demoMotion: demoMotion,
    toast: F.ui.toast.show
  });
  const api = F.api.bind(handlers);

  labels.ensure(state.doc);
  F.render.fitCamera(state.doc, state.cam, true);
  syncDocHeader();
  tree.render();
  inspector.render();
  controls.sync();
  telemetry.resize();
  setKernelPill(false);

  $("fileDoc").onchange = function (e) {
    if (e.target.files[0]) loadDoc(e.target.files[0]);
    e.target.value = "";
  };
  $("fileModel").onchange = function (e) {
    loadModelFiles(e.target.files);
    e.target.value = "";
  };
  const bodyPreset = $("bodyPreset");
  if (bodyPreset) {
    bodyPreset.onchange = function () {
      if (bodyPreset.value === "custom") return;
      replaceDocument(F.model.presetDoc(Number(bodyPreset.value)), true);
    };
  }
  window.addEventListener("resize", function () { telemetry.resize(); });

  F.app.startFrameLoop({
    state: state,
    renderer: renderer,
    telemetry: telemetry,
    bridge: state.bridge,
    inspector: inspector,
    tree: tree,
    demoMotion: demoMotion
  });
})(window.AEVUM.FORGE);
