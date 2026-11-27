/* Model catalog: known glTF assets and URL loading. No UI, no control. */
(function (F) {
  "use strict";

  const CATALOG = {
    toy_offroad: {
      label: "TOY OFFROAD",
      url: "../models/vehicle/toy_offroad.gltf",
      file: "toy_offroad.gltf"
    },
    arm3: {
      label: "3R ARM",
      url: "../models/arm/arm3.gltf",
      file: "arm3.gltf"
    }
  };

  const DEFAULT_KEY = "toy_offroad";

  function keyFromLocation() {
    try {
      const key = new URLSearchParams(window.location.search).get("model");
      return key && CATALOG[key] ? key : DEFAULT_KEY;
    } catch (e) {
      return DEFAULT_KEY;
    }
  }

  function load(key) {
    const entry = CATALOG[key];
    if (!entry) return Promise.reject(new Error("未知模型: " + key));
    const url = new URL(entry.url, document.baseURI).href;
    return fetch(url, { cache: "no-cache" }).then(function (response) {
      if (!response.ok) throw new Error("HTTP " + response.status + " / " + url);
      return response.arrayBuffer();
    }).then(function (buffer) {
      return F.model.importGltfBuffer(buffer, entry.file);
    });
  }

  F.model.MODEL_CATALOG = CATALOG;
  F.model.DEFAULT_MODEL_KEY = DEFAULT_KEY;
  F.model.modelKeyFromLocation = keyFromLocation;
  F.model.loadModel = load;
})(window.AEVUM.FORGE);
