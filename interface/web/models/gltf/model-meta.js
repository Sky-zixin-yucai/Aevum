/*
 * 职责唯一：只负责 Aevum 模型元数据的公共语义。
 *
 * 不认车辆、不认机械臂，也不负责 glTF buffer/accessor。
 * 公共字段：schema、kind、placeholder、units、coordinate_system、source、
 * joints、control、sensors。kind 专属字段由各自模型模块校验。
 */
"use strict";

const SCHEMA = "aevum.model.gltf/1";
const KINDS = ["arm", "vehicle"];

function coordinateSystem() {
  return { forward: "x", up: "y", right: "z", ground_plane: "xz" };
}

function fail(message) {
  throw new Error("aevum.model: " + message);
}

function isVec3(value) {
  return Array.isArray(value) && value.length === 3 && value.every(Number.isFinite);
}

function validateJoint(joint) {
  if (!joint || typeof joint.id !== "string" || !joint.id) fail("joint id 不能为空");
  if (typeof joint.type !== "string" || !joint.type) fail("joint type 不能为空: " + joint.id);
  if (typeof joint.parent !== "string" || !joint.parent) fail("joint parent 不能为空: " + joint.id);
  if (!isVec3(joint.axis)) fail("joint axis 必须是 3 个数: " + joint.id);
  if (!(Number.isFinite(joint.min) && Number.isFinite(joint.max) && Number.isFinite(joint.home))) fail("joint min/max/home 必须是数字: " + joint.id);
  if (!(joint.min <= joint.home && joint.home <= joint.max)) fail("joint home 不在 min/max 内: " + joint.id);
  if (typeof joint.channel !== "string" || !joint.channel) fail("joint channel 不能为空: " + joint.id);
}

function validate(gltf, expectedKind) {
  const aevum = gltf?.asset?.extras?.aevum;
  if (!aevum) fail("缺少 asset.extras.aevum");
  if (aevum.schema !== SCHEMA) fail("schema 必须是 " + SCHEMA);
  if (KINDS.indexOf(aevum.kind) < 0) fail("kind 必须是 " + KINDS.join(" / "));
  if (expectedKind && aevum.kind !== expectedKind) fail("kind 必须是 " + expectedKind);
  if (typeof aevum.placeholder !== "boolean") fail("placeholder 必须是布尔值");
  if (aevum.units !== "m") fail("units 必须是 m");

  const cs = aevum.coordinate_system;
  if (!cs || cs.forward !== "x" || cs.up !== "y" || cs.right !== "z" || cs.ground_plane !== "xz") {
    fail("坐标约定必须是 x 向前、y 向上、z 向右、地面 xz");
  }

  if (!aevum.source || typeof aevum.source.kind !== "string") fail("source.kind 不能为空");
  if (!Array.isArray(aevum.joints) || !aevum.joints.length) fail("缺少 joints");

  const jointIds = {};
  aevum.joints.forEach(function (joint) {
    validateJoint(joint);
    if (jointIds[joint.id]) fail("joint id 重复: " + joint.id);
    jointIds[joint.id] = true;
  });

  if (!aevum.control || !(aevum.control.loop_hz > 0) || !(aevum.control.dt > 0)) fail("control.loop_hz/dt 必须是正数");
  if (!aevum.control.channels || typeof aevum.control.channels !== "object") fail("缺少 control.channels");
  Object.keys(aevum.control.channels).forEach(function (name) {
    const list = aevum.control.channels[name];
    if (!Array.isArray(list)) fail("control.channels." + name + " 必须是数组");
    list.forEach(function (jointId) {
      if (!jointIds[jointId]) fail("control.channels." + name + " 引用了不存在的 joint: " + jointId);
    });
  });
  if (!aevum.control.limits || typeof aevum.control.limits !== "object") fail("缺少 control.limits");
  if (aevum.sensors && typeof aevum.sensors !== "object") fail("sensors 必须是对象");

  gltf.nodes.forEach(function (node) {
    if (!node.extras || !node.extras.aevum || typeof node.extras.aevum.role !== "string") {
      fail("node " + (node.name || "?") + " 缺少 extras.aevum.role");
    }
  });

  return true;
}

module.exports = {
  SCHEMA,
  KINDS,
  coordinateSystem,
  validate
};
