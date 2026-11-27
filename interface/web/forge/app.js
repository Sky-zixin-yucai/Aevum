/* Root composition: model load, UI wiring, bridge. The page never integrates or drives the model. */
(function (F) {
  "use strict";

  const $ = F.dom.$;

  const initialDoc = F.model.defaultDoc();
  const state = {
    doc: initialDoc,
    control: initialDoc,
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

  function controlTip() {
    const chain = F.model.controlSegments(state.control);
    const baseH = Number(state.control.base && state.control.base.h) || 0.24;
    let theta = 0;
    let x = 0;
    let y = baseH;
    chain.forEach(function (seg) {
      theta += Number(seg.q) || 0;
      const len = F.model.controlLength(state.control, seg);
      x += len * Math.cos(theta);
      y += len * Math.sin(theta);
    });
    return { x: x, y: y, theta: theta };
  }

  function updateCarWheels(pose) {
    if (!state.doc || modelKind(state.doc) === "arm") return;
    const segs = state.doc.segments || [];
    const last = state.avatarLastPose;
    if (last) {
      const dist = Math.hypot(pose.x - last.x, pose.z - last.z);
      /* 轮半径 0.04 m；滚动角只由车体位移推导，不另造一套车辆状态。 */
      state.avatarSpin = (Number(state.avatarSpin) || 0) + dist / 0.04;
    }
    state.avatarLastPose = { x: pose.x, z: pose.z, yaw: pose.yaw };
    segs.forEach(function (seg) {
      if (seg.jointType === "spin") seg.q = -(Number(state.avatarSpin) || 0);
      if (seg.jointType === "steer") seg.q = Number(state.avatarSteer) || 0;
    });
  }

  function applyAvatarPose() {
    if (!state.doc || state.doc === state.control) return;
    const chain = F.model.controlSegments(state.control);
    if (!chain.length) return;
    const tip = controlTip();
    const anchor = state.avatarAnchor || (state.avatarAnchor = controlTip());
    const pose = state.doc.pose || (state.doc.pose = { x: 0, z: 0, yaw: 0 });
    /* 车辆驾驶使用游戏位姿（在键盘层按 W/S/A/D 积分），3R 只负责内核 q；
     * 这样车头、位移和方向盘是同一套坐标，不会被 3R 的阻尼残差带成弧线。 */
    if (state.avatarDriving && state.avatarPose) {
      pose.x = state.avatarPose.x;
      pose.z = state.avatarPose.z;
      pose.yaw = state.avatarPose.yaw;
      updateCarWheels(pose);
      return;
    }
    /* 车/导入模型在第一次按 WASD 之前固定为零；否则内核还在追旧 q_target，
     * 车会自己漂到画面外。第一次按键时由键盘层锁当前 q 作为 anchor。 */
    if (!state.avatarDriving) {
      pose.x = 0;
      pose.z = 0;
      pose.yaw = 0;
      state.avatarSpin = 0;
      state.avatarLastPose = null;
      updateCarWheels(pose);
      return;
    }
    if (!state.avatarAnchor) {
      state.avatarAnchor = controlTip();
      pose.x = 0;
      pose.z = 0;
      pose.yaw = 0;
      updateCarWheels(pose);
      return;
    }
    /* 模型是 3R 基座上的外观：只取相对 home 的末端位移；车头角取 3R 第 3 个自由度。
     * toy_offroad 的坐标约定是 x 向前、z 侧向，所以 x 用 tip.x、z 用 tip.y。 */
    pose.x = tip.x - anchor.x;
    pose.z = tip.y - anchor.y;
    /* 车头方向以真实位移为准。3R 的阻尼/限幅会让实际 q 有残差，
     * 若继续用 tip.theta 当车头，车身会朝一边、实际位移在另一边。
     * 用上一帧到这一帧的真实位移反推航向，车头就必然和运动方向一致；
     * 倒车时仍保留车头向前。 */
    const prev = state.avatarLastPose;
    const mdx = prev ? pose.x - prev.x : 0;
    const mdz = prev ? pose.z - prev.z : 0;
    if (prev && Math.hypot(mdx, mdz) > 1e-4) {
      const sign = Number(state.avatarDriveSign) < 0 ? -1 : 1;
      pose.yaw = sign > 0 ? Math.atan2(-mdz, mdx) : Math.atan2(mdz, -mdx);
    } else {
      pose.yaw = tip.theta - anchor.theta;
    }
    state.avatarLastPose = { x: pose.x, z: pose.z };
    updateCarWheels(pose);
    /* 相机不锁在车体上：锁死会让车永远停在画面中央，看起来像没动。 */
  }
  state.controlTip = controlTip;

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
    const joints = doc.aevum && doc.aevum.joints ? doc.aevum.joints.length :
      segs.filter(function (s) { return s.actuated; }).length;
    const aevum = doc.aevum || {};

    if (docName) docName.textContent = doc.name || "MODEL";
    if (kind === "vehicle") {
      const wheels = aevum.wheels && aevum.wheels.length || 0;
      if (docMeta) docMeta.textContent = wheels + " wheels · " + tris + " tris · placeholder";
      if (title) title.textContent = "TOY OFFROAD / MODEL VIEW";
      if (sub) sub.textContent = "glTF 2.0 · vehicle placeholder · kernel external";
    } else if (kind === "arm") {
      const reach = (aevum.body && aevum.body.links || []).reduce(function (sum, l) { return sum + (Number(l.length) || 0); }, 0) ||
        segs.reduce(function (sum, s) { return sum + (Number(s.length) || 0); }, 0);
      if (docMeta) docMeta.textContent = joints + " joints · " + tris + " tris · " + reach.toFixed(2) + " m";
      if (title) title.textContent = "3R ARM / MODEL VIEW";
      if (sub) sub.textContent = "planar 3R base · W/A/S/D kernel target · no model-defined commands";
    } else {
      if (docMeta) docMeta.textContent = nodes + " nodes · " + tris + " tris";
      if (title) title.textContent = "IMPORTED / " + (doc.name || "MODEL");
      if (sub) sub.textContent = "glTF 2.0 · " + segs.length + " nodes · " + tris + " tris · view only";
    }
  }

  function replaceDocument(doc, notify) {
    const next = F.model.validate(doc);
    state.doc = next;
    if (modelKind(next) === "arm") state.control = next;
    if (!state.control) state.control = F.model.defaultDoc();
    state.selected = { kind: "base", index: -1 };
    if (modelKind(next) !== "arm") {
      /* 模型对象会被 catalog 缓存，必须清掉上一轮的 pose 和轮角。 */
      next.pose = { x: 0, z: 0, yaw: 0 };
      next.guide = null;
      (next.segments || []).forEach(function (seg) {
        if (seg.jointType === "steer" || seg.jointType === "spin") seg.q = 0;
      });
    }
    state.sim = F.control.createSession(next);
    state.avatarAnchor = modelKind(next) === "arm" ? null : controlTip();
    state.avatarDriving = false;
    state.avatarPose = null;
    state.avatarSpin = 0;
    state.avatarSteer = 0;
    state.avatarDriveSign = 1;
    state.avatarLastPose = null;
    applyAvatarPose();
    labels.ensure(state.doc);
    Object.assign(state.cam, F.render.createCamera());
    state.cam.mode = "orbit";
    F.render.fitCamera(state.doc, state.cam, true);
    tree.render();
    inspector.render();
    if (workbench) workbench.sync();
    if (status) {
      status.sync();
      status.setKernel(false);
    }
    telemetry.resize();
    syncDocHeader();
    const preset = $("modelPreset");
    if (preset) {
      const file = state.doc.source && state.doc.source.file || "";
      const key = Object.keys(F.model.MODEL_CATALOG || {}).find(function (k) {
        return F.model.MODEL_CATALOG[k].file === file;
      });
      if (key) preset.value = key;
    }
    if (state.bridge.online && bridgeControl) bridgeControl.sendBody();
    if (notify) {
      const meta = state.doc.meta || {};
      F.ui.toast.show("已加载 " + state.doc.name + " · " + state.doc.segments.length + " nodes / " +
        (meta.triangleCount || 0) + " tris");
    }
  }

  function loadModelKey(key, notify) {
    if (!key || key === "custom") return;
    F.model.loadModel(key).then(function (doc) {
      replaceDocument(doc, notify);
    }).catch(function (e) {
      F.ui.toast.show("模型加载失败: " + e.message + "；用 Import glTF 选择文件");
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
    files.forEach(function (file) {
      F.dom.readFileBuffer(file, function (err, buffer) {
        if (err) {
          F.ui.toast.show("读取失败: " + file.name + " / " + err.message);
          return;
        }
        loaded.push({ name: file.name, data: buffer });
        if (loaded.length === files.length) {
          try {
            replaceDocument(F.model.importGltfFiles(loaded, main.name), true);
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
    onControlUpdate: applyAvatarPose,
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
    onToggleBridge: bridgeControl.toggle,
    onBodyChange: function () { if (state.bridge.online) bridgeControl.sendBody(); }
  });

  F.app.createKeyboardControl({
    state: state,
    bridgeControl: bridgeControl,
    toast: F.ui.toast.show,
    onCarSteer: function (pose, steer) {
      if (state.doc && state.doc !== state.control) {
        state.doc.pose = pose;
        updateCarWheels(pose);
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

  const debugPanel = F.ui.createDebugPanel({
    state: state,
    box: $("debugBody"),
    now: $("debugTick")
  });
  setInterval(function () { debugPanel.update(); }, 250);

  workbench = F.app.createWorkbench({
    state: state,
    canvas: canvas,
    renderer: renderer,
    labels: labels,
    onSelect: function (kind, index) { tree.select(kind, index); }
  });

  const handlers = F.app.createApiHandlers({
    state: state,
    inspector: inspector,
    replaceDocument: replaceDocument,
    bridgeControl: bridgeControl,
    toast: F.ui.toast.show
  });
  const api = F.api.bind(handlers);

  const modelPreset = $("modelPreset");
  if (modelPreset) {
    modelPreset.onchange = function () { loadModelKey(modelPreset.value, true); };
  }
  const fileModel = $("fileModel");
  if (fileModel) {
    fileModel.onchange = function (e) {
      loadModelFiles(e.target.files);
      e.target.value = "";
    };
  }
  const importTop = $("btnImportTop");
  if (importTop && fileModel) importTop.onclick = function () { fileModel.click(); };

  function boot() {
    labels.ensure(state.doc);
    F.render.fitCamera(state.doc, state.cam, true);
    syncDocHeader();
    tree.render();
    inspector.render();
    status.sync();
    telemetry.resize();
    if (bridgeControl) bridgeControl.connect();
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
