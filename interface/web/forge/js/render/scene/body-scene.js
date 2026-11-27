/* Imported kinematic tree: BodyDocument v1 → world matrices + joint anchors. */
(function (F) {
  "use strict";

  const M = F.math;
  F.render = F.render || {};

  function buildBodyScene(doc) {
    const links = [];
    const joints = [];
    const world = [];

    function rootMatrix(seg) {
      const pose = doc.pose || { x: 0, z: 0, yaw: 0 };
      const base = M.mul(M.trans(pose.x || 0, 0, pose.z || 0), M.axisAngle([0, 1, 0], pose.yaw || 0));
      return childMatrix(seg, base);
    }

    function childMatrix(seg, parentWorld) {
      const j = seg.joint;
      let m = parentWorld;
      if (j) {
        m = M.mul(m, M.trans(j.origin[0], j.origin[1], j.origin[2]));
        m = M.mul(m, M.axisAngle(j.axis, seg.q || 0));
      }
      return M.mul(m, M.trs(seg.local.translation, seg.local.rotation, seg.local.scale));
    }

    function anchor(seg, parentWorld) {
      if (seg.joint) {
        const p = parentWorld
          ? M.mul(parentWorld, M.trans(seg.joint.origin[0], seg.joint.origin[1], seg.joint.origin[2]))
          : M.trans(seg.joint.origin[0], seg.joint.origin[1], seg.joint.origin[2]);
        return M.translation(p);
      }
      return M.translation(parentWorld || M.mat4());
    }

    function walk(index, parentWorld) {
      const seg = doc.segments[index];
      if (!seg || world[index]) return;
      const m = parentWorld ? childMatrix(seg, parentWorld) : rootMatrix(seg);
      world[index] = m;
      links[index] = {
        seg: seg,
        model: m,
        normal: M.mat3(m),
        mesh: seg.mesh || null
      };
      joints[index] = {
        id: seg.id,
        pos: anchor(seg, parentWorld),
        axis: seg.joint ? seg.joint.axis : [0, 0, 1],
        q: seg.q || 0
      };
      doc.segments.forEach(function (s, i) {
        if (s.parent === index) walk(i, m);
      });
    }

    doc.segments.forEach(function (s, i) {
      if (s.parent < 0 || !doc.segments[s.parent]) walk(i, null);
    });
    /* 防止 parent 断裂/环导致空数组；未走到的节点按根处理。 */
    doc.segments.forEach(function (s, i) {
      if (!world[i]) walk(i, null);
    });

    return {
      mode: "body",
      links: links.filter(Boolean),
      joints: joints.filter(Boolean),
      end: M.translation(world[world.length - 1] || M.mat4()),
      endMat: world[world.length - 1] || M.mat4()
    };
  }

  function buildScene(doc) {
    if (F.model.isImported(doc)) return buildBodyScene(doc);
    return F.render.buildArmScene(doc);
  }

  function linkBounds(link) {
    if (!link.mesh) {
      const seg = link.seg || {};
      const c = M.translation(link.model);
      const half = [Math.max(0.04, (seg.length || 0.1)) / 2, Math.max(0.02, (seg.width || 0.06)) / 2, Math.max(0.02, (seg.depth || 0.06)) / 2];
      const mn = [c[0] - half[0], c[1] - half[1], c[2] - half[2]];
      const mx = [c[0] + half[0], c[1] + half[1], c[2] + half[2]];
      const size = [mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]];
      return {
        min: mn,
        max: mx,
        center: c,
        size: size,
        span: Math.max(size[0], size[1], size[2])
      };
    }
    const localMin = link.mesh.bounds && link.mesh.bounds.min || [-0.5, -0.5, -0.5];
    const localMax = link.mesh && link.mesh.bounds && link.mesh.bounds.max || [0.5, 0.5, 0.5];
    const mn = [Infinity, Infinity, Infinity];
    const mx = [-Infinity, -Infinity, -Infinity];
    for (let i = 0; i < 8; i++) {
      const p = M.transformPoint(link.model, [
        (i & 1) ? localMax[0] : localMin[0],
        (i & 2) ? localMax[1] : localMin[1],
        (i & 4) ? localMax[2] : localMin[2]
      ]);
      for (let c = 0; c < 3; c++) {
        mn[c] = Math.min(mn[c], p[c]);
        mx[c] = Math.max(mx[c], p[c]);
      }
    }
    const size = [mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]];
    return {
      min: mn,
      max: mx,
      center: [(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2],
      size: size,
      span: Math.max(size[0], size[1], size[2])
    };
  }

  function sceneBounds(scene) {
    const mn = [Infinity, Infinity, Infinity];
    const mx = [-Infinity, -Infinity, -Infinity];
    (scene.links || []).forEach(function (link) {
      if (!link) return;
      const b = linkBounds(link);
      for (let c = 0; c < 3; c++) {
        mn[c] = Math.min(mn[c], b.min[c]);
        mx[c] = Math.max(mx[c], b.max[c]);
      }
    });
    if (!isFinite(mn[0])) return null;
    return {
      min: mn,
      max: mx,
      center: [(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2],
      size: [mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]],
      span: Math.max(mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2], 0.3)
    };
  }

  F.render.buildBodyScene = buildBodyScene;
  F.render.buildScene = buildScene;
  F.render.linkBounds = linkBounds;
  F.render.sceneBounds = sceneBounds;
})(window.AEVUM.FORGE);
