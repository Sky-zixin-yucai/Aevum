/* Root composition: model load, UI wiring, bridge. The page never integrates or drives the model. */
(function (F) {
  "use strict";

  const $ = F.dom.$;

  const state = {
    doc: F.model.defaultDoc(),
    selected: { kind: "base", index: -1 },
    sim: null,
    cam: F.render.createCamera(),
    bridge: F.bridge.create("ws://127.0.0.1:8765")
  };
  state.sim = F.control.createSession(state.doc);

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
  let status = null;
  let telemetry = null;
  let workbench = null;
  let bridgeControl = null;

  function modelKind(doc) {
    return doc && doc.modelKind || (F.model.isImported(doc) ? "generic" : "arm");
  }

  function catalogKey(doc) {
    const file = doc && doc.source && doc.source.file || "";
    const entries = F.model.MODEL_CATALOG;
    return Object.keys(entries).find(function (key) { return entries[key].file === file; }) || "custom";
  }

  function firstActuated(doc) {
    for (let i = 0; i < doc.segments.length; i++) {
      if (doc.segments[i].actuated) return i;
    }
    return -1;
  }

  function syncDocHeader() {
    const doc = state.doc;
    const title = $("hudTitle");
    const sub = document.querySelector(".hud .sub");
    const docName = $("docName");
    const docMeta = $("docMeta");
    const kind = modelKind(doc);
    const segs = doc.segments || [];
    const meta = doc.meta || {};
    const tris = meta.triangleCount || 0;
    const nodes = doc.source && doc.source.nodes || segs.length;
    const joints = doc.aevum && doc.aevum.joints ? doc.aevum.joints.length : 0;
    const aevum = doc.aevum || {};

    if (docName) docName.textContent = doc.name || "MODEL";
    if (kind === "vehicle") {
      const wheels = aevum.wheels && aevum.wheels.length || 0;
      if (docMeta) docMeta.textContent = wheels + " wheels · " + tris + " tris · placeholder";
      if (title) title.textContent = "TOY OFFROAD / MODEL VIEW";
      if (sub) sub.textContent = "glTF 2.0 · vehicle placeholder · kernel external";
    } else if (kind === "arm") {
      const reach = (aevum.body && aevum.body.links || []).reduce(function (sum, l) { return sum + (Number(l.length) || 0); }, 0);
      if (docMeta) docMeta.textContent = joints + " joints · " + tris + " tris · " + reach.toFixed(2) + " m";
      if (title) title.textContent = "3R ARM / MODEL VIEW";
      if (sub) sub.textContent = "glTF 2.0 · " + joints + " revolute joints · kernel external";
    } else {
      if (docMeta) docMeta.textContent = nodes + " nodes · " + tris + " tris";
      if (title) title.textContent = "IMPORTED / " + (doc.name || "MODEL");
      if (sub) sub.textContent = "glTF 2.0 · " + segs.length + " nodes · " + tris + " tris · view only";
    }

    const preset = $("modelPreset");
    if (preset) {
      const key = catalogKey(doc);
      const custom = preset.querySelector('option[value="custom"]');
      if (key !== "custom") {
        if (custom) {
          custom.disabled = true;
          custom.textContent = "IMPORTED";
        }
        preset.value = key;
      } else {
        if (custom) {
          custom.disabled = false;
          custom.textContent = "IMPORTED / " + (doc.source && doc.source.file || "GLTF");
        }
        preset.value = "custom";
      }
    }
  }

  function replaceDocument(doc, notify) {
    state.doc = F.model.validate(doc);
    const key = catalogKey(state.doc);
    if (key !== "custom" && F.model.MODEL_CATALOG[key]) state.doc.name = F.model.MODEL_CATALOG[key].label;
    state.selected = { kind: "base", index: -1 };
    state.sim = F.control.createSession(state.doc);
    labels.ensure(state.doc);
    Object.assign(state.cam, F.render.createCamera());
    F.render.fitCamera(state.doc, state.cam, true);
    tree.render();
    inspector.render();
    if (status) {
      status.sync();
      status.setKernel(modelKind(state.doc) === "vehicle" ? "model" : false);
    }
    telemetry.resize();
    syncDocHeader();
    if (notify) {
      const meta = state.doc.meta || {};
      F.ui.toast.show("已加载 " + state.doc.name + " · " + state.doc.segments.length + " nodes / " +
        (meta.triangleCount || 0) + " tris");
    }
  }

  function loadModelKey(key, notify) {
    if (!key || key === "custom") return;
    F.ui.toast.show("加载模型 " + key + "…");
    F.model.loadModel(key).then(function (doc) {
      replaceDocument(doc, notify);
    }).catch(function (e) {
      F.ui.toast.show("模型加载失败: " + e.message + "；用 Import glTF 手动选择文件");
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
            replaceDocument(F.model.importGltfFiles(loaded, main.name), false);
            F.ui.toast.show("已导入 " + main.name + " · " + state.doc.segments.length + " nodes / " +
              ((state.doc.meta && state.doc.meta.triangleCount) || 0) + " tris");
          } catch (e) {
            F.ui.toast.show("glTF 导入失败: " + e.message);
          }
        }
      });
    });
  }

  status = F.ui.createStatus({ state: state });

  bridgeControl = F.app.createBridgeControl({
    state: state,
    setKernelPill: status.setKernel,
    rerenderInspector: function () { if (inspector) inspector.render(); },
    syncControls: status.sync,
    onKernelState: function () { if (tree) tree.render(); },
    updateKernelStatus: function () { if (inspector) inspector.updateRuntime(); },
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
    onImport: function () { $("fileModel").click(); },
    onToggleBridge: bridgeControl.toggle,
    onBodyChange: function () { if (state.bridge.online) bridgeControl.sendBody(); },
    onKernelTarget: bridgeControl.pulseManual,
    onPathLine: function (params) {
      if (!bridgeControl.sendPath(params)) {
        F.ui.toast.show("LINE 未发送：内核桥未连接");
      }
    },
    onPathArc: function (params) {
      if (!bridgeControl.sendPath(params)) {
        F.ui.toast.show("ARC 未发送：内核桥未连接");
      }
    }
  });

  telemetry = F.ui.createTelemetryPanel({
    state: state,
    chart: $("chart"),
    onMetrics: function (metrics) {
      api._emit({
        type: "telemetry",
        kernel: !!state.sim.kernel,
        manual: !!state.sim.manual,
        metrics: metrics,
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
    onImport: function () { $("fileModel").click(); }
  });

  const handlers = F.app.createApiHandlers({
    state: state,
    inspector: inspector,
    replaceDocument: replaceDocument,
    bridgeControl: bridgeControl,
    toast: F.ui.toast.show
  });
  const api = F.api.bind(handlers);

  function boot() {
    labels.ensure(state.doc);
    F.render.fitCamera(state.doc, state.cam, true);
    syncDocHeader();
    tree.render();
    inspector.render();
    status.sync();
    telemetry.resize();
    const key = F.model.modelKeyFromLocation();
    loadModelKey(key, false);
  }

  $("fileModel").onchange = function (e) {
    loadModelFiles(e.target.files);
    e.target.value = "";
  };
  const modelPreset = $("modelPreset");
  if (modelPreset) {
    modelPreset.onchange = function () {
      if (modelPreset.value === "custom") return;
      loadModelKey(modelPreset.value, true);
    };
  }
  window.addEventListener("resize", function () { telemetry.resize(); });

  F.app.startFrameLoop({
    state: state,
    renderer: renderer,
    telemetry: telemetry,
    bridge: state.bridge,
    inspector: inspector,
    tree: tree
  });

  boot();
})(window.AEVUM.FORGE);
