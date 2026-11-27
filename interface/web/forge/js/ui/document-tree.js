/* Left-side model tree. A glTF node is a part; only nodes listed by the model become joints. */
(function (F) {
  "use strict";

  const fmt = F.math.fmt;
  const R2D = F.math.R2D;

  function createDocumentTree(opts) {
    const state = opts.state;
    const treeEl = opts.tree;
    const purposeEl = opts.purpose;
    const selIdEl = opts.selId;
    const searchEl = opts.search || null;
    const onSelect = opts.onSelect || function () {};

    function filter(query) {
      const q = String(query || "").trim().toLowerCase();
      let shown = 0;
      treeEl.querySelectorAll(".tree-item").forEach(function (el) {
        const hit = !q || el.textContent.toLowerCase().indexOf(q) >= 0;
        el.style.display = hit ? "" : "none";
        if (hit) shown++;
      });
      let empty = treeEl.querySelector(".tree-empty");
      if (!shown && q) {
        if (!empty) {
          empty = document.createElement("div");
          empty.className = "tree-empty";
          treeEl.appendChild(empty);
        }
        empty.textContent = "No node or joint matches “" + q + "”.";
      } else if (empty) {
        empty.remove();
      }
    }

    if (searchEl) {
      searchEl.addEventListener("input", function () { filter(searchEl.value); });
    }

    function detailFor(s) {
      const tris = s.mesh ? s.mesh.triangles + " tris" : "no mesh";
      if (!s.actuated) return "part · " + tris;
      const kind = s.jointType === "prismatic" ? "prismatic" : s.jointType || "joint";
      return kind + " · " + (s.channel || "-") + " · q " + fmt((s.q || 0) * R2D, 0) + "°";
    }

    function depthOf(index) {
      let depth = 0;
      let p = state.doc.segments[index] && state.doc.segments[index].parent;
      while (p >= 0 && depth < 10) {
        depth++;
        p = state.doc.segments[p] ? state.doc.segments[p].parent : -1;
      }
      return depth;
    }

    function kindLabel(doc) {
      if (doc.modelKind === "vehicle") return "VEHICLE";
      if (doc.modelKind === "arm") return "3R ARM";
      return doc.modelKind === "generic" ? "GENERIC" : "MODEL";
    }

    function render() {
      const doc = state.doc;
      const selected = state.selected;
      const actuated = doc.segments.filter(function (s) { return s.actuated; }).length;
      let html = '<div class="tree-item ' + (selected.kind === "base" ? "active" : "") + '" data-kind="base" data-index="-1">'
        + '<span class="icon">S</span>'
        + '<span class="tag">SCENE</span><div class="k">' + (doc.name || "MODEL") + '</div><div class="v">'
        + kindLabel(doc) + ' · ' + doc.segments.length + ' nodes · ' + actuated + ' joints</div></div>';

      doc.segments.forEach(function (s, i) {
        const active = selected.kind === "joint" && selected.index === i;
        const depth = depthOf(i);
        const icon = s.actuated ? "J" : "P";
        const tag = s.actuated ? "JOINT" : "PART";
        html += '<div class="tree-item ' + (active ? "active" : "") + '" data-kind="joint" data-index="' + i + '" style="padding-left:' + (6 + depth * 14) + 'px">'
          + '<span class="icon">' + icon + '</span><span class="tag">' + (s.semanticId || s.id) + '</span>'
          + '<div class="k">' + (s.name || s.id) + '</div>'
          + '<div class="v" data-q="' + i + '">' + detailFor(s) + '</div></div>';
      });

      treeEl.innerHTML = html;
      treeEl.querySelectorAll(".tree-item").forEach(function (el) {
        el.onclick = function () {
          select(el.dataset.kind, Number(el.dataset.index));
        };
      });
      filter(searchEl ? searchEl.value : "");
      updatePurpose();
    }

    function select(kind, index) {
      state.selected = { kind: kind, index: index };
      render();
      onSelect(kind, index);
    }

    function updatePurpose() {
      const stat = document.getElementById("statSelection");
      if (state.selected.kind === "base") {
        purposeEl.innerHTML = "<b>Scene / " + kindLabel(state.doc) + "</b>glTF 场景。静态节点只呈现外观；只有模型声明的关节交给内核。";
        selIdEl.textContent = "SCENE";
        if (stat) stat.textContent = "SCENE";
        return;
      }
      const s = state.doc.segments[state.selected.index];
      if (!s) return;
      if (stat) stat.textContent = s.semanticId || s.id;
      if (s.actuated) {
        purposeEl.innerHTML = "<b>" + (s.semanticId || s.id) + " / " + (s.jointType || "joint") + "</b>"
          + " parent " + (s.parent < 0 ? "SCENE" : "node " + state.doc.segments[s.parent].id)
          + " · channel " + (s.channel || "-")
          + " · <span class=\"accent\">kernel controls</span>";
      } else {
        purposeEl.innerHTML = "<b>" + s.id + " / static part</b>几何与材质节点，不产生关节，不接受运动目标。";
      }
      selIdEl.textContent = s.semanticId || s.id;
    }

    function updateRuntime() {
      state.doc.segments.forEach(function (s, i) {
        const el = treeEl.querySelector('[data-q="' + i + '"]');
        if (el) el.textContent = detailFor(s);
      });
    }

    return { render: render, select: select, updatePurpose: updatePurpose, updateRuntime: updateRuntime };
  }

  F.ui.createDocumentTree = createDocumentTree;
})(window.AEVUM.FORGE);
