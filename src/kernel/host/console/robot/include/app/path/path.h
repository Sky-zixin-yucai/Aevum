#ifndef AEVUM_APP_PATH_PATH_H
#define AEVUM_APP_PATH_PATH_H

#include "kernel/types.h"
#include "lib/geom/line.h"
#include "lib/geom/arc.h"

/*
 * 内核几何路径执行器：1D 有界线段（LINE）和有界圆弧（ARC）。
 *
 * 这里只有几何参数和运动参数，没有“车辆/油门/转向”语义：
 *   几何：线段端点；圆弧圆心、半径、起始角、带符号扫角；
 *   运动：速度、容忍带、超时；
 *   约束：速度上限。
 * 每周期只推进弧长 s，再用 t=s/L 从几何族求路径点、切向和曲率。
 * 身体层/3D 被控对象自己把路径量映射成轮子或关节，内核不存动作表。
 */
#define PATH_KIND_NONE 0
#define PATH_KIND_LINE 1
#define PATH_KIND_ARC  2

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

struct path_arc_params {
    i32 cx;      /* 圆心，Q16.16 m */
    i32 cz;
    i32 r;       /* 半径，Q16.16 m，必须 > 0 */
    i32 a0;      /* 起始角，Q16.16 rad */
    i32 sweep;   /* 带符号扫角，Q16.16 rad；正为 x-z 平面逆时针 */
    i32 speed;   /* Q16.16 m/s, >= 0 */
    i32 tol;     /* Q16.16 m */
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
    i32 kappa;      /* 车体 yaw 对 s 的曲率，Q16.16 1/m；LINE 为 0 */
    i32 speed;
    i32 tol;
};

/* 返回 1 表示已接受；0 表示参数非法，状态会停在 FAULT 供状态帧回传。 */
int path_line_start(const struct path_line_params *params);
int path_arc_start(const struct path_arc_params *params);

/* 通用入口：kind + 8 个 Q16.16 字段；字段顺序与 UART P 命令一致。 */
int path_start(int kind, const i32 *values, int count);
void path_step(void);

/* 有状态（RUN/DONE/FAULT）时返回 1；IDLE 返回 0。 */
int path_has_status(void);
void path_status_get(struct path_status *out);

#endif
