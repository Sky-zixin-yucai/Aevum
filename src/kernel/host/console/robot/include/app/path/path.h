#ifndef AEVUM_APP_PATH_PATH_H
#define AEVUM_APP_PATH_PATH_H

#include "kernel/types.h"
#include "lib/geom/line.h"

/*
 * 内核几何路径执行器：1D 有界线段（LINE）。
 *
 * 这里只有几何参数和运动参数，没有“车辆/油门/转向”语义：
 *   几何：p0/p1 两个端点；
 *   运动：速度、容忍带、超时；
 *   约束：速度上限。
 * 每周期只推进弧长 s，再用 t=s/L 从 line2 求路径点和切向。
 * 身体层/3D 被控对象自己把路径点映射成轮子或关节，内核不存动作表。
 */
#define PATH_KIND_NONE 0
#define PATH_KIND_LINE 1

#define PATH_STATE_IDLE  0
#define PATH_STATE_RUN   1
#define PATH_STATE_DONE  2
#define PATH_STATE_FAULT 3

#define PATH_FAULT_NONE    0
#define PATH_FAULT_GEOM    1   /* 退化线段 */
#define PATH_FAULT_SPEED   2   /* 速度为负或超过上限 */
#define PATH_FAULT_TIMEOUT 3   /* 运行超过动作实例给的时限 */

#define PATH_DT 655            /* 100 Hz, Q16.16 秒 */
#define PATH_MAX_SPEED 327680  /* 5 m/s */
#define PATH_DEFAULT_TIMEOUT 6000

struct path_line_params {
    i32 x0;      /* Q16.16 m */
    i32 z0;
    i32 x1;
    i32 z1;
    i32 speed;   /* Q16.16 m/s, >= 0 */
    i32 tol;     /* Q16.16 m, 到位容忍带；当前只回传，不改变几何 */
    i32 timeout; /* 控制周期数；<=0 用默认值 */
};

struct path_status {
    int kind;
    int state;
    int fault;
    i32 tick;       /* 已执行的周期数 */
    i32 s;          /* 已走弧长，Q16.16 m */
    i32 t;          /* 归一化弧长，Q16.16 */
    i32 length;     /* 线段长度，Q16.16 m */
    i32 x;          /* 当前路径点 */
    i32 z;
    i32 dx;         /* 切向单位向量，Q16.16 */
    i32 dz;
    i32 speed;
    i32 tol;
};

/* 返回 1 表示已接受；0 表示参数非法，状态会停在 FAULT 供状态帧回传。 */
int path_line_start(const struct path_line_params *params);
void path_step(void);

/* 有状态（RUN/DONE/FAULT）时返回 1；IDLE 返回 0。 */
int path_has_status(void);
void path_status_get(struct path_status *out);

#endif
