(() => {
  'use strict';

  const DURATION = 1000;
  const CFS_LATENCY = 10;
  const CFS_MIN_GRANULARITY = 1;
  const A0_SLICE = 4;
  const COLORS = ['#8fbf7a', '#d6a45c', '#7ea6d8', '#d47a63'];

  const canvas = document.getElementById('arena');
  const ctx = canvas.getContext('2d');
  const rows = document.getElementById('rows');
  const hint = document.getElementById('hint');
  const legend = document.getElementById('legend');
  const statPolicy = document.getElementById('stat-policy');
  const statTicks = document.getElementById('stat-ticks');
  const statSwitches = document.getElementById('stat-switches');
  const statWait = document.getElementById('stat-wait');

  const state = {
    taskCount: 4,
    policy: 'a0',
    cursor: 0,
    playing: false,
    speed: 3,
    tasks: [],
    timeline: [],
    switches: 0,
    maxWait: 0,
    rowH: 30,
    logicalW: 1000,
    logicalH: 460
  };

  function weightsFor(policy, priority) {
    if (policy === 'a0') return priority + 1;
    if (policy === 'fifo') return 1;
    return 1 << (3 - priority);
  }

  function sliceFor(policy, weight, totalWeight) {
    if (policy === 'a0') return A0_SLICE;
    return Math.max(CFS_MIN_GRANULARITY, Math.floor(CFS_LATENCY * weight / totalWeight));
  }

  function prioritiesFor(count) {
    if (count === 2) return [0, 3];
    const out = [];
    const per = Math.max(1, Math.floor(count / 4));
    for (let p = 0; p < 4; p++) {
      for (let i = 0; i < per; i++) out.push(p);
    }
    while (out.length < count) out.push(out.length % 4);
    return out.slice(0, count);
  }

  function build() {
    const priorities = prioritiesFor(state.taskCount);
    const tasks = priorities.map((priority, index) => ({
      id: `T${index}`,
      priority,
      weight: weightsFor(state.policy, priority),
      vruntime: 0,
      cpu: 0,
      wait: 0,
      maxWait: 0,
      color: COLORS[priority]
    }));
    const totalWeight = tasks.reduce((sum, task) => sum + task.weight, 0);
    const isFifo = state.policy === 'fifo';
    let timeline = [];
    let switches = 0;
    let lastTask = null;
    let tick = 0;

    if (isFifo) {
      // Linux SCHED_FIFO：高优先级不阻塞就一直占 CPU；同优先级按到达顺序。
      // 这里全是 CPU-bound，所以最高优先级任务跑满整个窗口，其余任务饿死。
      const winner = tasks.reduce((best, task) => {
        if (!best) return task;
        if (task.priority < best.priority) return task;
        if (task.priority === best.priority && task.id < best.id) return task;
        return best;
      }, null);
      winner.cpu = DURATION;
      for (const task of tasks) {
        if (task === winner) continue;
        task.wait = DURATION;
        task.maxWait = DURATION;
      }
      timeline.push({ tid: winner.id, start: 0, len: DURATION, priority: winner.priority });
    } else {
      while (tick < DURATION) {
        const current = tasks.reduce((best, task) => {
          if (!best) return task;
          if (task.vruntime < best.vruntime) return task;
          if (task.vruntime === best.vruntime && task.id < best.id) return task;
          return best;
        }, null);

        const run = Math.min(sliceFor(state.policy, current.weight, totalWeight), DURATION - tick);
        for (const task of tasks) {
          if (task === current) continue;
          task.wait += run;
          task.maxWait = Math.max(task.maxWait, task.wait);
        }

        current.cpu += run;
        current.wait = 0;
        current.vruntime += run * 64 / current.weight;
        if (lastTask && lastTask !== current) switches++;
        lastTask = current;

        const last = timeline[timeline.length - 1];
        if (last && last.tid === current.id && last.start + last.len === tick) {
          last.len += run;
        } else {
          timeline.push({ tid: current.id, start: tick, len: run, priority: current.priority });
        }
        tick += run;
      }
    }

    state.tasks = tasks;
    state.timeline = timeline;
    state.switches = switches;
    state.maxWait = Math.max(...tasks.map(task => task.maxWait), 0);
    state.cursor = 0;

    const totalCpu = tasks.reduce((sum, task) => sum + task.cpu, 0);
    statPolicy.textContent = state.policy === 'a0' ? 'A0' : (isFifo ? 'FIFO' : 'CFS');
    statSwitches.textContent = switches;
    statWait.textContent = state.maxWait;
    statTicks.textContent = `0 / ${DURATION}`;

    rows.innerHTML = tasks.map(task => {
      const share = totalCpu ? task.cpu / totalCpu : 0;
      const theory = isFifo
        ? (task.cpu > 0 ? 1 : 0)
        : (totalWeight ? task.weight / totalWeight : 0);
      return `<tr>
        <td>${task.id}</td>
        <td>${task.priority}</td>
        <td>${isFifo ? '—' : task.weight}</td>
        <td>${task.cpu}</td>
        <td>${(share * 100).toFixed(1)}%</td>
        <td>${(theory * 100).toFixed(1)}%</td>
        <td>${task.maxWait}</td>
      </tr>`;
    }).join('');

    hint.textContent = state.policy === 'a0'
      ? 'A0 用当前代码的 weight = priority + 1；priority 0 只有 weight 1，和文档“0..3 -> 4..1”相反。'
      : (isFifo
        ? 'Linux SCHED_FIFO：高优先级不阻塞就独占 CPU；这里全是 CPU-bound，低优先级任务会饿死。'
        : 'CFS 目标用 weight = 8/4/2/1，片长按总权重算；这里只模拟 CPU-bound，不含阻塞和唤醒。');

    legend.innerHTML = COLORS.map((color, priority) =>
      `<span><i class="swatch" style="background:${color}"></i>priority ${priority}</span>`
    ).join('');

    resize();
  }

  function resize() {
    const box = canvas.parentElement;
    const dpr = window.devicePixelRatio || 1;
    state.logicalW = Math.max(640, box.clientWidth - 24);
    state.logicalH = 80 + state.taskCount * 34 + 40;
    canvas.style.height = `${state.logicalH}px`;
    canvas.width = Math.round(state.logicalW * dpr);
    canvas.height = Math.round(state.logicalH * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  function draw() {
    const width = state.logicalW;
    const height = state.logicalH;
    const left = 132;
    const right = 18;
    const top = 52;
    const plotW = width - left - right;
    const rowH = Math.min(34, (height - top - 34) / Math.max(1, state.taskCount));

    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = '#1f1d19';
    ctx.fillRect(0, 0, width, height);

    ctx.fillStyle = '#9a917f';
    ctx.font = '12px "Segoe UI", "Microsoft YaHei", sans-serif';
    ctx.fillText('tick', 12, 24);
    for (let t = 0; t <= DURATION; t += 100) {
      const x = left + t / DURATION * plotW;
      ctx.strokeStyle = t % 500 === 0 ? '#5d5446' : '#34302a';
      ctx.beginPath();
      ctx.moveTo(x, top - 6);
      ctx.lineTo(x, top + state.taskCount * rowH);
      ctx.stroke();
      ctx.fillStyle = '#9a917f';
      ctx.fillText(String(t), x + 3, 24);
    }

    for (let i = 0; i < state.tasks.length; i++) {
      const task = state.tasks[i];
      const y = top + i * rowH;
      ctx.fillStyle = '#d8cfbb';
      ctx.font = '12px "Consolas", monospace';
      const weight = state.policy === 'fifo' ? '—' : `w${task.weight}`;
      ctx.fillText(`${task.id}  P${task.priority}  ${weight}`, 12, y + rowH / 2 + 4);
      ctx.strokeStyle = '#302c26';
      ctx.beginPath();
      ctx.moveTo(left, y + rowH);
      ctx.lineTo(width - right, y + rowH);
      ctx.stroke();
    }

    for (const block of state.timeline) {
      const x = left + block.start / DURATION * plotW;
      const w = Math.max(1, block.len / DURATION * plotW);
      const y = top + state.tasks.findIndex(task => task.id === block.tid) * rowH + 5;
      ctx.fillStyle = COLORS[block.priority];
      ctx.globalAlpha = 0.1;
      ctx.fillRect(x, y, w, rowH - 10);

      const playedEnd = Math.min(block.start + block.len, state.cursor);
      if (playedEnd > block.start) {
        const playedW = Math.max(1, (playedEnd - block.start) / DURATION * plotW);
        ctx.globalAlpha = 0.55;
        ctx.fillRect(x, y, playedW, rowH - 10);
      }
      ctx.globalAlpha = 1;
    }

    const playX = left + state.cursor / DURATION * plotW;
    ctx.strokeStyle = '#e8e0cf';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(playX, top - 10);
    ctx.lineTo(playX, top + state.taskCount * rowH);
    ctx.stroke();
    ctx.fillStyle = '#e8e0cf';
    ctx.fillRect(playX - 2, top - 12, 4, 4);

    ctx.fillStyle = '#9a917f';
    ctx.font = '12px "Segoe UI", "Microsoft YaHei", sans-serif';
    ctx.fillText(`cursor ${Math.round(state.cursor)} / ${DURATION}`, left, height - 12);
  }

  function update(dt) {
    if (!state.playing) return;
    state.cursor += dt * 100 * state.speed;
    if (state.cursor >= DURATION) {
      state.cursor = DURATION;
      state.playing = false;
    }
    statTicks.textContent = `${Math.round(state.cursor)} / ${DURATION}`;
  }

  function bind() {
    document.getElementById('tasks').addEventListener('change', event => {
      state.taskCount = Number(event.target.value);
      state.playing = false;
      build();
    });
    document.getElementById('policy').addEventListener('change', event => {
      state.policy = event.target.value;
      state.playing = false;
      build();
    });
    document.getElementById('speed').addEventListener('input', event => {
      state.speed = Number(event.target.value);
    });
    document.getElementById('run').addEventListener('click', () => {
      if (state.cursor >= DURATION) state.cursor = 0;
      state.playing = true;
    });
    document.getElementById('pause').addEventListener('click', () => {
      state.playing = false;
    });
    document.getElementById('reset').addEventListener('click', () => {
      state.playing = false;
      state.cursor = 0;
      statTicks.textContent = `0 / ${DURATION}`;
    });
    window.addEventListener('resize', resize);
  }

  function startEngine() {
    if (!window.OrbEngine || !window.OrbEngine.engineLoop) {
      // 引擎未加载时仍然能画静态图，避免页面空白。
      draw();
      return;
    }
    window.OrbEngine.init({ targetFPS: 60, debug: false }).then(() => {
      window.OrbEngine.engineLoop.onUpdate(update);
      window.OrbEngine.engineLoop.onRender(draw);
      window.OrbEngine.startLoop();
    }).catch(error => {
      console.error('[arena] engine init failed', error);
      draw();
    });
  }

  bind();
  build();
  startEngine();
})();
