#ifndef AEVUM_LIB_GEOM_LINE_H
#define AEVUM_LIB_GEOM_LINE_H

#include "kernel/types.h"

/*
 * 1D 几何族：平面有界线段。
 * 没有速度、时间、曲率、朝向语义；t 是归一化弧长参数，不是时间。
 * 坐标和 t 都是 Q16.16；t 只在 [0, FXONE] 内有效。
 */
struct line2 {
    i32 x0;
    i32 z0;
    i32 x1;
    i32 z1;
};

/* 线段长度的平方，Q32.32；退化线段返回 0。 */
i64 line2_len_sq(const struct line2 *l);

/* 线段长度，Q16.16；退化线段返回 0。 */
i32 line2_length(const struct line2 *l);

/* p(t) = p0 + t * (p1 - p0)。t 越界或退化线段返回 0，否则返回 1。 */
int line2_point(const struct line2 *l, i32 t, i32 *x, i32 *z);

/*
 * 点在直线上的投影参数 t。
 * 返回 1 表示 t 在 [0, FXONE] 内，0 表示在端点外，-1 表示退化线段。
 * 即使返回 0，*t 仍是投影参数，供调用者判断方向。
 */
int line2_project(const struct line2 *l, i32 px, i32 pz, i32 *t);

#endif
