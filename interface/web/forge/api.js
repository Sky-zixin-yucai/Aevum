/* Public API surface. Commands and snapshots only; no renderer or DOM implementation. */
(function (F) {
  "use strict";

  let handlers = null;
  const listeners = [];

  function requireHandlers() {
    if (!handlers) throw new Error("Forge API is not bound yet");
    return handlers;
  }

  const api = {
    version: "0.2.0",
    model: "glTF 2.0",
    viewOnly: true,
    bind: function (h) {
      handlers = h;
      return api;
    },
    getState: function () { return requireHandlers().getState(); },
    getDocument: function () { return requireHandlers().getDocument(); },
    setDocument: function (doc) { return requireHandlers().setDocument(doc); },
    setTarget: function (index, degrees) { return requireHandlers().setTarget(index, degrees); },
    setTargets: function (degrees) { return requireHandlers().setTargets(degrees); },
    setManual: function (on) { return requireHandlers().setManual(on); },
    importModel: function (name, buffer) { return requireHandlers().importModel(name, buffer); },
    getTelemetry: function () { return requireHandlers().getTelemetry(); },
    connect: function (url) { return requireHandlers().connect(url); },
    disconnect: function () { return requireHandlers().disconnect(); },
    runLine: function (params) { return requireHandlers().runLine(params); },
    subscribe: function (fn) {
      listeners.push(fn);
      return function () {
        const i = listeners.indexOf(fn);
        if (i >= 0) listeners.splice(i, 1);
      };
    },
    _emit: function (data) {
      listeners.forEach(function (fn) {
        try { fn(data); } catch (e) { /* listener must not break the loop */ }
      });
    }
  };

  F.api = api;
  window.AEVUM_FORGE = api;
})(window.AEVUM.FORGE);
