#ifndef AEVUM_LIB_GEOM_ARC_H
#define AEVUM_LIB_GEOM_ARC_H

#include "kernel/types.h"

/*
 * 1D 几何族：平面有界圆弧。
 * 角度和坐标都是 Q16.16；a0/sweep 是弧度，sweep 带符号。
 * 圆心 + 半径 + 起始角 + 扫角，不包含速度、时间或车辆语义。
 * t 是归一化弧长参数，不是时间。
 */
struct arc2 {
    i32 cx;
    i32 cz;
    i32 r;
    i32 a0;
    i32 sweep;
};

/* 圆弧长度，Q16.16 m；半径非正或扫角为零返回 0。 */
i32 arc2_length(const struct arc2 *a);

/* p(t) = center + r * (cos(a0 + sweep*t), sin(...))。t 越界或退化返回 0。 */
int arc2_point(const struct arc2 *a, i32 t, i32 *x, i32 *z);

/* 单位切向；扫角为正时逆时针，为负时顺时针。退化返回 0。 */
int arc2_tangent(const struct arc2 *a, i32 t, i32 *dx, i32 *dz);

/*
 * 车体 yaw 对弧长 s 的曲率，Q16.16 1/m。
 * 正扫角得到 -1/r，负扫角得到 +1/r；退化返回 0。
 */
i32 arc2_curvature(const struct arc2 *a);

#endif
