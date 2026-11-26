/* 100 Hz PD plant. Pure math: no DOM, no WebGL, no bridge side effects. */
(function (F) {
  "use strict";

  const clamp = F.math.clamp;
  const R2D = F.math.R2D;
  const D2R = F.math.D2R;

  function createSim(doc) {
    return {
      running: false,
      sine: false,
      kernel: false,
      manual: false,
      manualSince: 0,
      demoDrive: false,
      kernelStatus: null,
      kernelRejected: false,
      time: 0,
      last: performance.now(),
      acc: 0,
      q: doc.segments.map(function (s) { return s.q; }),
      qd: doc.segments.map(function () { return 0; }),
      errHist: [],
      qHist: [],
      lastUi: 0,
      lastSend: 0,
      seq: 0
    };
  }

  function reset(doc, sim) {
    doc.segments.forEach(function (s, i) {
      s.q = s.target;
      sim.q[i] = s.q;
      sim.qd[i] = 0;
    });
    sim.time = 0;
    sim.acc = 0;
    sim.errHist = [];
    sim.last = performance.now();
    sim.lastSend = 0;
    sim.manual = false;
  }

  function tick(doc, sim, now, send) {
    const dt = 1 / (doc.control.loop_hz || 100);
    if (!sim.last) sim.last = now;
    let elapsed = (now - sim.last) / 1000;
    sim.last = now;
    elapsed = Math.min(elapsed, 0.05);
    sim.acc += elapsed;

    let n = 0;
    while (sim.acc >= dt && n < 4) {
      const t = sim.time;
      for (let i = 0; i < doc.segments.length; i++) {
        const s = doc.segments[i];
        let target = s.target;
        if (sim.sine && i === 1) target = 0.6 * Math.sin(t * Math.PI * 2 * 0.55);
        if (sim.sine && i === 2) target = -0.35 + 0.35 * Math.sin(t * Math.PI * 2 * 0.55 + 0.8);
        const err = target - sim.q[i];
        let tau = s.kp * err - s.kd * sim.qd[i];
        tau = clamp(tau, -s.torque, s.torque);
        const qdd = (tau - s.damping * sim.qd[i]) / s.inertia;
        sim.qd[i] += qdd * dt;
        sim.q[i] += sim.qd[i] * dt;

        const lo = s.min * D2R;
        const hi = s.max * D2R;
        if (sim.q[i] < lo) { sim.q[i] = lo; sim.qd[i] = 0; }
        if (sim.q[i] > hi) { sim.q[i] = hi; sim.qd[i] = 0; }
        s.q = sim.q[i];

        sim.errHist.push(Math.abs(err * R2D));
        if (sim.errHist.length > 360) sim.errHist.shift();
      }
      sim.time += dt;
      sim.acc -= dt;
      n++;
    }

    if (send && now - sim.lastSend > 100) {
      sim.lastSend = now;
      send({
        type: "command",
        seq: ++sim.seq,
        target: doc.segments.map(function (s) { return +s.target.toFixed(5); }),
        q: doc.segments.map(function (s) { return +s.q.toFixed(5); })
      });
    }
  }

  F.control = F.control || {};
  F.control.createSim = createSim;
  F.control.reset = reset;
  F.control.tick = tick;
})(window.AEVUM.FORGE);
