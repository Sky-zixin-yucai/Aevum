/* Screen-space joint labels; projection and hit points live together. */
(function (F) {
  "use strict";

  function createJointLabels(container, canvas) {
    let labelEls = {};
    let screenJoints = [];
    let screenParts = [];

    function ensure(doc) {
      container.innerHTML = "";
      labelEls = {};
      for (let i = 0; i < doc.segments.length; i++) {
        const seg = doc.segments[i];
        if (!seg.actuated) continue;
        const parent = seg.parent >= 0 ? doc.segments[seg.parent] : null;
        if (parent && parent.jointType === "steer" && seg.jointType === "spin") continue;
        const d = document.createElement("div");
        d.className = "joint-label";
        d.innerHTML = '<span class="id">' + (seg.semanticId || seg.id) + '</span><span class="val"></span>';
        container.appendChild(d);
        labelEls[seg.id] = d;
      }
      screenJoints = [];
      screenParts = [];
    }

    function project(vp, p) {
      const c = [
        vp[0] * p[0] + vp[4] * p[1] + vp[8] * p[2] + vp[12],
        vp[1] * p[0] + vp[5] * p[1] + vp[9] * p[2] + vp[13],
        vp[2] * p[0] + vp[6] * p[1] + vp[10] * p[2] + vp[14],
        vp[3] * p[0] + vp[7] * p[1] + vp[11] * p[2] + vp[15]
      ];
      if (c[3] <= 0.001) return null;
      const w = canvas.clientWidth;
      const h = canvas.clientHeight;
      return {
        x: (c[0] / c[3] * 0.5 + 0.5) * w,
        y: (1 - (c[1] / c[3] * 0.5 + 0.5)) * h
      };
    }

    function update(scene, vp, selected) {
      const w = canvas.clientWidth;
      const h = canvas.clientHeight;
      screenJoints = [];
      for (let i = 0; i < scene.joints.length; i++) {
        const j = scene.joints[i];
        const link = scene.links[i];
        if (link && link.seg && !link.seg.actuated) continue;
        const p = project(vp, j.pos);
        const el = labelEls[j.id];
        if (!el) continue;
        if (!p || p.x < -80 || p.x > w + 80 || p.y < -50 || p.y > h + 50) {
          el.style.display = "none";
          continue;
        }
        el.style.display = "block";
        el.style.left = p.x + "px";
        el.style.top = p.y + "px";
        el.classList.toggle("sel", selected.kind === "joint" && selected.index === i);
        const val = el.querySelector(".val");
        if (val) val.textContent = F.math.fmt((j.q || 0) * F.math.R2D, 0) + "°";
        screenJoints.push({ index: i, x: p.x, y: p.y });
      }
      screenParts = [];
      for (let k = 0; k < scene.links.length; k++) {
        const link = scene.links[k];
        if (!link || !link.seg || !link.seg.mesh) continue;
        const p = project(vp, F.math.translation(link.model));
        if (!p || p.x < 0 || p.x > w || p.y < 0 || p.y > h) continue;
        screenParts.push({ index: k, x: p.x, y: p.y });
      }
      return screenJoints;
    }

    function joints() {
      return screenJoints;
    }

    function parts() {
      return screenParts;
    }

    return { ensure: ensure, update: update, project: project, joints: joints, parts: parts };
  }

  F.render.createJointLabels = createJointLabels;
})(window.AEVUM.FORGE);
