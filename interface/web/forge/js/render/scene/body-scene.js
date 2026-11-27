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
        /* 用节点实际世界矩阵，不能用 anchor(seg,null)：根节点的 anchor 会落回原点，
         * 车移动时标签就固定在世界原点。 */
        pos: M.translation(m),
        axis: seg.joint ? seg.joint.axis : [0, 0, 1],
        q: seg.q || 0
      };
      /* 轮关节锚点在轮心；标签在 screen-space 抬到轮子上方，见 joint-labels.js。 */
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
      guides: doc.guide && doc.guide.length >= 6
        ? [{ points: doc.guide, color: [0.95, 0.64, 0.30] }]
        : [],
      end: M.translation(world[world.length - 1] || M.mat4()),
      endMat: world[world.length - 1] || M.mat4()
    };
  }

  /*
   * 平面机械臂按内核 body.links 的顺序重算世界矩阵。
   * glTF 里的节点可能只是平铺的外观件（节点原点在世界坐标），不能再拿节点
   * 自身的 translation/rotation 当关节层级，否则 q 变化时只会原地旋转。
   */
  function buildPlanarScene(doc) {
    const chain = F.model.controlSegments(doc);
    if (!chain.length) return buildBodyScene(doc);
    const base = doc.base || { w: 0.54, h: 0.24, d: 0.38 };
    const tool = doc.tool || { length: 0.2, width: 0.075, depth: 0.09 };
    const links = [];
    const joints = [];
    let m = M.trans(0, base.h, 0);

    chain.forEach(function (seg) {
      const len = F.model.controlLength(doc, seg);
      const width = Math.max(0.02, Number(seg.width) || 0.1);
      const depth = Math.max(0.02, Number(seg.depth) || 0.1);
      joints.push({
        id: seg.id,
        pos: M.translation(m),
        axis: [0, 0, 1],
        q: seg.q || 0
      });
      const rot = M.mul(m, M.rz(seg.q || 0));
      links.push({
        seg: seg,
        model: M.mul(M.mul(rot, M.trans(len / 2, 0, 0)), M.scale(len, width, depth)),
        normal: M.mat3(rot),
        mesh: seg.mesh || null
      });
      m = M.mul(rot, M.trans(len, 0, 0));
    });

    const baseSeg = doc.segments.find(function (s) {
      return s.semanticId === "base" || s.role === "base";
    });
    if (baseSeg && baseSeg.mesh) {
      links.push({
        seg: baseSeg,
        model: M.mul(M.trans(0, base.h / 2, 0), M.scale(base.w, base.h, base.d)),
        normal: M.mat3(M.mat4()),
        mesh: baseSeg.mesh
      });
    }

    const toolSeg = doc.segments.find(function (s) {
      return s.semanticId === "tool" || s.role === "tool";
    });
    if (toolSeg && toolSeg.mesh) {
      links.push({
        seg: toolSeg,
        model: M.mul(M.mul(m, M.trans(tool.length / 2, 0, 0)),
          M.scale(tool.length, tool.width, tool.depth)),
        normal: M.mat3(m),
        mesh: toolSeg.mesh
      });
    }

    return {
      mode: "body",
      links: links,
      joints: joints,
      guides: [],
      end: M.translation(m),
      endMat: m
    };
  }

  function buildScene(doc) {
    if (doc && doc.modelKind === "arm" && doc.aevum && doc.aevum.body) {
      return buildPlanarScene(doc);
    }
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
