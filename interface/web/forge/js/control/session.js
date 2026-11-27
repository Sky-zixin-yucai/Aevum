/* Session state shared by UI and bridge. It does not integrate or drive the model. */
(function (F) {
  "use strict";

  function createSession(doc) {
    return {
      kernel: false,
      manual: false,
      manualSince: 0,
      kernelStatus: null,
      path: null,
      pathCmd: null,
      kernelRejected: false,
      bodyPending: false,
      bodySentAt: 0,
      q: doc.segments.map(function (s) { return s.q; }),
      qd: doc.segments.map(function () { return 0; }),
      qHist: [],
      lastUi: 0
    };
  }

  function resetSession(doc, session) {
    session.q = doc.segments.map(function (s) { return s.q; });
    session.qd = doc.segments.map(function () { return 0; });
    session.qHist.length = 0;
  }

  F.control = F.control || {};
  F.control.createSession = createSession;
  F.control.resetSession = resetSession;
})(window.AEVUM.FORGE);
