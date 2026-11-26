/* Parameterized demo motion: sweep and a simple 3R pick. Host-side only. */
(function (F) {
  "use strict";

  const M = F.math;

  /*
   * 这一层不定义动作库：它只把“左右摆动”和“3R 抓取”各展开成一组
   * 关节目标参数。内核看到的是普通 J 命令；离线时由本地 PD 跟踪。
   */
  function createDemoMotion(opts) {
    const state = opts.state;
    const bridge = opts.bridgeControl;
    const syncControls = opts.syncControls || function () {};
    const toast = opts.toast || function () {};

    function kernel() {
      return !!state.sim.kernel;
    }

    function beginDrive() {
      if (kernel()) {
        state.sim.demoDrive = true;
        bridge.setManual(true);
        return;
      }
      state.sim.running = true;
      state.sim.last = performance.now();
      state.sim.acc = 0;
    }

    function holdAtCurrent() {
      state.doc.segments.forEach(function (s, i) {
        s.target = state.sim.q[i] == null ? s.q : state.sim.q[i];
      });
    }

    function frameHeld() {
      if (!state.cam) return;
      const prop = state.doc.prop;
      const cx = prop ? Number(prop.pos[0]) - 0.28 : 0.58;
      F.render.cameraFrameAt(state.cam, [cx, 0.12, 0], 2.0);
    }

    function startSweep() {
      if (!state.doc.segments.length) return;
      state.demo.grabbing = false;
      state.demo.grabAt = 0;
      state.demo.grabBy = 0;
      state.demo.sweepBase = state.sim.q[0] == null
        ? state.doc.segments[0].q
        : state.sim.q[0];
      beginDrive();
      state.demo.sweepStart = performance.now();
      state.demo.sweep = true;
      syncControls();
    }

    function stopSweep(hold) {
      state.demo.sweep = false;
      if (hold) holdAtCurrent();
      syncControls();
    }

    function toggleSweep() {
      if (state.demo.sweep) stopSweep(true);
      else startSweep();
    }

    /*
     * 3R 抓取：工具保持水平，前两关节解到方块左侧面，第三关节回正。
     * 只服务当前默认 3R 示例；2R/4R–8R 仍保留建模和手动控制。
     */
    function solve3R() {
      const segs = state.doc.segments;
      const prop = state.doc.prop;
      if (segs.length !== 3 || !prop) return null;

      const size = prop.size || [0.12, 0.12, 0.12];
      const L1 = Number(segs[0].length) || 0;
      const L2 = Number(segs[1].length) || 0;
      const tool = Number(state.doc.tool && state.doc.tool.length) || 0;
      const baseY = Number(state.doc.base && state.doc.base.h) || 0;
      const tipX = Number(prop.pos[0]) - size[0] / 2;
      const tipY = Number(prop.pos[1]);
      const wx = tipX - tool;
      const wy = tipY;
      const dx = wx;
      const dy = wy - baseY;
      const r = Math.hypot(dx, dy);

      if (r > L1 + L2 - 0.001 || r < Math.abs(L1 - L2) + 0.001) return null;

      const c = (r * r - L1 * L1 - L2 * L2) / (2 * L1 * L2);
      const elbow = -Math.acos(M.clamp(c, -1, 1));
      const phi = Math.atan2(dy, dx);
      const shoulder = phi - Math.atan2(
        L2 * Math.sin(elbow),
        L1 + L2 * Math.cos(elbow)
      );
      return [shoulder, elbow, -(shoulder + elbow)];
    }

    function release() {
      state.demo.grabbing = false;
      state.demo.grabAt = 0;
      state.demo.grabBy = 0;
      state.sim.demoDrive = false;
      if (state.doc.prop) state.doc.prop.held = false;
      if (state.doc.tool) state.doc.tool.grip = 0;
      if (state.cam) F.render.fitCamera(state.doc, state.cam);
      syncControls();
    }

    function grab() {
      if (!state.doc.prop) {
        toast("场景里没有方块");
        return false;
      }
      if (state.doc.prop.held) {
        release();
        toast("RELEASE：方块已放回原位");
        return true;
      }
      if (state.doc.segments.length !== 3) {
        toast("抓取示例当前只支持 3R");
        return false;
      }

      const q = solve3R();
      if (!q) {
        toast("方块超出当前 3R 可达范围");
        return false;
      }

      stopSweep(false);
      beginDrive();
      state.doc.segments.forEach(function (s, i) {
        if (i < q.length) s.target = q[i];
      });
      state.demo.grabbing = true;
      state.demo.grabAt = performance.now() + 1200;
      state.demo.grabBy = performance.now() + 9000;
      syncControls();
      toast("GRAB：末端对准方块，等待夹爪闭合");
      return true;
    }

    function reset() {
      state.demo.sweep = false;
      state.demo.grabbing = false;
      state.demo.grabAt = 0;
      state.demo.grabBy = 0;
      release();
      syncControls();
    }

    function tick(now) {
      if (state.demo.sweep) {
        const s = state.doc.segments[0];
        if (s) {
          const t = (now - state.demo.sweepStart) / 1000;
          s.target = state.demo.sweepBase + 0.65 * Math.sin(t * Math.PI * 2 * 0.4);
        }
      }
      if (state.demo.grabbing && state.demo.grabAt && now >= state.demo.grabAt) {
        let err = 0;
        state.doc.segments.forEach(function (s) {
          const e = Math.abs((s.q == null ? 0 : s.q) - (s.target == null ? 0 : s.target));
          if (e > err) err = e;
        });
        if (err > 0.06 && now < state.demo.grabBy) return;
        state.demo.grabbing = false;
        state.demo.grabAt = 0;
        state.demo.grabBy = 0;
        if (state.doc.prop) state.doc.prop.held = true;
        if (state.doc.tool) state.doc.tool.grip = 1;
        frameHeld();
        syncControls();
        toast("GRAB 完成：方块已夹住");
      }
    }

    return {
      startSweep: startSweep,
      stopSweep: stopSweep,
      toggleSweep: toggleSweep,
      grab: grab,
      release: release,
      reset: reset,
      tick: tick,
      solve3R: solve3R
    };
  }

  F.control = F.control || {};
  F.control.createDemoMotion = createDemoMotion;
})(window.AEVUM.FORGE);
