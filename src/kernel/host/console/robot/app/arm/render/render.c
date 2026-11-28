#include "app/arm/render.h"
#include "app/arm/app.h"
#include "arch/x86/interrupt.h"
#include "drivers/video/vga.h"
#include "kernel/task.h"
#include "config/memory.h"
#include "lib/fixed.h"
#include "lib/format.h"
#include "lib/trig.h"

#define C_GRAY   0x08
#define C_DGRAY  0x07
#define C_WHITE  0x0F
#define C_GREEN  0x0A
#define C_RED    0x0C
#define C_YELLOW 0x0E
#define C_CYAN   0x0B
#define C_BLUE   0x09
#define C_MAG    0x0D

#define CH_H     0xC4
#define CH_V     0xB3
#define CH_TL    0xDA
#define CH_TR    0xBF
#define CH_BL    0xC0
#define CH_BR    0xD9
#define CH_TT    0xC2
#define CH_BT    0xC1
#define CH_FULL  0xDB
#define CH_MED   0xB1
#define CH_LIGHT 0xB0
#define CH_DEG   0xF8

#define CV_X  1
#define CV_Y  2
#define CV_W  42
#define CV_H  19

#define WX0    (-95027)
#define WY0    (-95027)
#define WSPANX 190054
#define WSPANY 190054

#define TRACE_N 48

/* 左上角工作空间小地图：固定 ±1.45 m；主画面也按同一范围画全工作空间。 */
#define MM_X  1
#define MM_Y  2
#define MM_W  13
#define MM_H  7
#define MM_WX0   (-95027)
#define MM_WY0   (-95027)
#define MM_SPAN  190054

static u16 trace[TRACE_N];
static int trace_i;
static u16 mtrace[TRACE_N];
static int mtrace_i;

static struct arm_points ghost_points;
static int ghost_n;
static int ghost_valid;
static int ghost_age;

void arm_render_init(void)
{
    int i;
    for (i = 0; i < TRACE_N; i++) {
        trace[i] = 0;
        mtrace[i] = 0;
    }
    trace_i = 0;
    mtrace_i = 0;
    ghost_valid = 0;
    ghost_n = 0;
    ghost_age = 0;
}

static int wx(i32 x)
{
    i32 t = fx_div(x - WX0, WSPANX);
    int c = CV_X + (int)(((i64)t * (CV_W - 1)) >> FXBITS);
    if (c < CV_X) c = CV_X;
    if (c > CV_X + CV_W - 1) c = CV_X + CV_W - 1;
    return c;
}

static int wy(i32 y)
{
    i32 t = fx_div(y - WY0, WSPANY);
    int r = CV_Y + CV_H - 1 - (int)(((i64)t * (CV_H - 1)) >> FXBITS);
    if (r < CV_Y) r = CV_Y;
    if (r > CV_Y + CV_H - 1) r = CV_Y + CV_H - 1;
    return r;
}

static int mx(i32 x)
{
    i32 t = fx_div(x - MM_WX0, MM_SPAN);
    int c = MM_X + 1 + (int)(((i64)t * (MM_W - 3)) >> FXBITS);
    if (c < MM_X + 1) c = MM_X + 1;
    if (c > MM_X + MM_W - 2) c = MM_X + MM_W - 2;
    return c;
}

static int my(i32 y)
{
    i32 t = fx_div(y - MM_WY0, MM_SPAN);
    int r = MM_Y + MM_H - 2 - (int)(((i64)t * (MM_H - 3)) >> FXBITS);
    if (r < MM_Y + 1) r = MM_Y + 1;
    if (r > MM_Y + MM_H - 2) r = MM_Y + MM_H - 2;
    return r;
}

static u8 task_pid_color(u32 pid)
{
    if (pid == 1) return C_CYAN;
    if (pid == 2) return C_MAG;
    if (pid == 3) return C_YELLOW;
    if (pid == 4) return C_BLUE;
    return C_GRAY;
}

static int fmt_compact(char *buf, u32 value)
{
    int p = 0;
    if (value >= 1000u) {
        p += fmt_dec(buf, value / 1000u);
        buf[p++] = 'k';
        buf[p] = 0;
        return p;
    }
    return fmt_dec(buf, value);
}

static void task_panel(void)
{
    char buf[40];
    struct task_info info[TASK_MAX];
    int slot, x, p, i, count, compact;
    int len;
    u32 switch_count = task_switch_total();
    u32 cpu_total = task_cpu_total();
    u32 now;

    vga_text(45, 18, "TASK SCHED", C_CYAN);
    p = 0;
    p += fmt_dec(buf, (u32)task_pid_used());
    buf[p++] = '/';
    p += fmt_dec(buf + p, (u32)task_pid_capacity());
    buf[p] = 0;
    vga_text(56, 18, "P", C_GRAY);
    vga_text(57, 18, buf, C_WHITE);
    vga_text(63, 18, "sw", C_GRAY);
    p = 0;
    p += fmt_compact(buf, switch_count);
    buf[p] = 0;
    vga_text(66, 18, buf, C_WHITE);

    vga_text(72, 18, "px", C_GRAY);
    p = 0;
    p += fmt_compact(buf, task_preempt_total());
    buf[p] = 0;
    vga_text(75, 18, buf, C_WHITE);

    count = 0;
    for (slot = 0; slot < TASK_MAX; slot++) {
        if (task_snapshot(slot, &info[count])) count++;
    }

    len = task_trace_len();
    now = len ? task_trace_pid(len - 1) : 0;

    compact = count > 3;
    x = 45;
    for (i = 0; i < count; i++) {
        u8 attr;
        u32 pct = cpu_total
            ? (info[i].cpu_ticks * 100u + cpu_total / 2u) / cpu_total
            : 0;
        if (pct > 100u) pct = 100u;
        p = 0;
        p += fmt_dec(buf + p, (u32)info[i].pid);
        buf[p++] = info[i].comm[0] ? info[i].comm[0] : '?';
        buf[p++] = info[i].comm[1] ? info[i].comm[1] : ' ';
        if (!compact) buf[p++] = ' ';
        buf[p++] = task_state_char(info[i].state);
        if (!compact) {
            buf[p++] = ' ';
            p += fmt_dec(buf + p, pct);
            buf[p++] = '%';
            buf[p++] = ' ';
        } else {
            p += fmt_dec(buf + p, pct);
            buf[p++] = '%';
        }
        buf[p] = 0;
        if ((u32)info[i].pid == now)
            attr = (u8)((task_pid_color(now) << 4) | C_WHITE);
        else if (info[i].state == TASK_RUNNING)
            attr = C_GREEN;
        else if (info[i].state == TASK_BLOCKED)
            attr = C_BLUE;
        else
            attr = C_GRAY;
        vga_text(x, 19, buf, attr);
        x += p;
        if (x > 76) break;
    }

    x = 45;
    for (i = 0; i < count; i++) {
        int n;
        p = 0;
        p += fmt_dec(buf + p, (u32)info[i].pid);
        buf[p++] = info[i].comm[0] ? info[i].comm[0] : '?';
        buf[p++] = info[i].comm[1] ? info[i].comm[1] : ' ';
        buf[p++] = ' ';
        buf[p] = 0;
        vga_text(x, 20, buf, C_GRAY);
        x += p;
        n = (int)((info[i].stack_used * 6u) / (AEVUM_TASK_STACK_USABLE / 8u));
        if (n > 6) n = 6;
        for (p = 0; p < 6; p++)
            vga_cell(x + p, 20, p < n ? (char)CH_FULL : (char)CH_LIGHT,
                     task_pid_color((u32)info[i].pid));
        x += 7;
        if (x > 76) break;
    }
}

static void task_timeline(void)
{
    int len = task_trace_len();
    int cells = len > 64 ? 64 : len;
    int first = len > 64 ? len - 64 : 0;
    int pad = (80 - cells) / 2;
    int c;

    for (c = 0; c < 80; c++) {
        if (c < pad || c >= pad + cells) {
            vga_cell(c, 21, ' ', C_GRAY);
        } else {
            u32 pid = task_trace_pid(first + c - pad);
            vga_cell(c, 21, (char)CH_FULL, task_pid_color(pid));
        }
    }
}

static void draw_map_trail(void)
{
    int i;
    for (i = 0; i < TRACE_N; i++) {
        u16 a = mtrace[(mtrace_i + i) % TRACE_N];
        u16 b = mtrace[(mtrace_i + i + 1) % TRACE_N];
        if (!a || !b) continue;
        vga_line(a & 0xFF, (int)(a >> 8), b & 0xFF, (int)(b >> 8),
                 '.', C_GRAY);
    }
}

static void draw_minimap(const struct arm_status *s, i32 vx_mm_s, i32 vy_mm_s)
{
    char buf[16];
    int c, r, i;

    /* 小地图清成黑底，避免主画面机械臂从下面穿出来。 */
    vga_fill(MM_X, MM_Y, MM_X + MM_W - 1, MM_Y + MM_H - 1, ' ', C_DGRAY);
    for (c = MM_X + 1; c <= MM_X + MM_W - 2; c++) {
        vga_cell(c, MM_Y, (char)CH_H, C_GRAY);
        vga_cell(c, MM_Y + MM_H - 1, (char)CH_H, C_GRAY);
    }
    for (r = MM_Y + 1; r <= MM_Y + MM_H - 2; r++) {
        vga_cell(MM_X, r, (char)CH_V, C_GRAY);
        vga_cell(MM_X + MM_W - 1, r, (char)CH_V, C_GRAY);
    }
    vga_cell(MM_X, MM_Y, (char)CH_TL, C_GRAY);
    vga_cell(MM_X + MM_W - 1, MM_Y, (char)CH_TR, C_GRAY);
    vga_cell(MM_X, MM_Y + MM_H - 1, (char)CH_BL, C_GRAY);
    vga_cell(MM_X + MM_W - 1, MM_Y + MM_H - 1, (char)CH_BR, C_GRAY);

    /* ±1.45 m 的固定参考系，base 永远在中心。 */
    for (c = MM_X + 1; c <= MM_X + MM_W - 2; c++)
        vga_cell(c, MM_Y + MM_H / 2, '.', C_GRAY);
    for (r = MM_Y + 1; r <= MM_Y + MM_H - 2; r++)
        vga_cell(MM_X + MM_W / 2, r, '.', C_GRAY);

    for (i = 0; i < TRACE_N; i++) {
        u16 t = mtrace[i];
        if (t) vga_cell(t & 0xFF, (int)(t >> 8), '*', C_GRAY);
    }
    draw_map_trail();

    vga_cell(mx(s->target.x), my(s->target.y), 'o', C_YELLOW);
    vga_cell(mx(s->points.p[s->n].x), my(s->points.p[s->n].y), '@', C_GREEN);
    vga_cell(MM_X + MM_W / 2, MM_Y + MM_H / 2, '#', C_WHITE);

    vga_cell(MM_X + MM_W / 2, MM_Y + 1, 'U', C_WHITE);
    vga_cell(MM_X + MM_W / 2, MM_Y + MM_H - 2, 'D', C_WHITE);
    vga_cell(MM_X + 1, MM_Y + MM_H / 2, 'L', C_WHITE);
    vga_cell(MM_X + MM_W - 2, MM_Y + MM_H / 2, 'R', C_WHITE);

    vga_text(MM_X, MM_Y - 1, "WS MAP 1.45m", C_GRAY);

    vga_text(MM_X, MM_Y + MM_H, "vx", C_GRAY);
    fmt_fx(buf, vx_mm_s, 0);
    vga_text(MM_X + 3, MM_Y + MM_H, buf, C_WHITE);
    vga_text(MM_X + 9, MM_Y + MM_H, "mm/s", C_GRAY);
    vga_text(MM_X, MM_Y + MM_H + 1, "vy", C_GRAY);
    fmt_fx(buf, vy_mm_s, 0);
    vga_text(MM_X + 3, MM_Y + MM_H + 1, buf, C_WHITE);
    vga_text(MM_X + 9, MM_Y + MM_H + 1, "mm/s", C_GRAY);
}

static void draw_ghost_pose(void)
{
    int c0, r0, c1, r1, i;

    if (!ghost_valid) return;
    c0 = wx(ghost_points.p[0].x);
    r0 = wy(ghost_points.p[0].y);
    for (i = 0; i < ghost_n; i++) {
        c1 = wx(ghost_points.p[i + 1].x);
        r1 = wy(ghost_points.p[i + 1].y);
        vga_line(c0, r0, c1, r1, (char)CH_LIGHT, C_DGRAY);
        c0 = c1;
        r0 = r1;
    }
}


#define DUEL_W    36
#define DUEL_A_X  2
#define DUEL_B_X  41
#define DUEL_TAPE 80

static void duel_bar(int x, int row, u32 step, u8 color)
{
    int n = (int)((step % (u32)(DUEL_W * 64)) / 64u);
    int i;
    if (n < 0) n = 0;
    if (n > DUEL_W) n = DUEL_W;
    for (i = 0; i < DUEL_W; i++)
        vga_cell(x + i, row, i < n ? (char)CH_FULL : (char)CH_LIGHT, color);
}

void arm_render_duel_init(void)
{
    int c;
    vga_clear(0x00);
    vga_text(17, 0, "AEVUM / PREEMPTION DUEL", 0x1F);
    vga_text(8, 2, "TWO CPU-BOUND TASKS / NEITHER CALLS task_yield()", 0x1B);
    vga_text(DUEL_A_X, 4, "TASK A", C_CYAN);
    vga_text(DUEL_B_X, 4, "TASK B", C_MAG);
    vga_text(DUEL_A_X, 8, "RUN HISTORY", C_GRAY);
    vga_text(DUEL_B_X, 8, "RUN HISTORY", C_GRAY);
    vga_text(3, 11, "PREEMPTION TAPE / WHICH TASK OWNS THE CPU", C_GRAY);
    vga_text(2, 16, "PIT PREEMPTIONS", C_GRAY);
    vga_text(2, 17, "A STEPS", C_GRAY);
    vga_text(41, 17, "B STEPS", C_GRAY);
    vga_text(2, 19, "T = STOP DUEL", 0x1E);
    vga_text(2, 21, "BOTH LANES MOVE ONLY BECAUSE IRQ0 PREEMPTS", 0x18);
    for (c = 0; c < DUEL_W; c++) {
        vga_cell(DUEL_A_X + c, 6, (char)CH_LIGHT, C_DGRAY);
        vga_cell(DUEL_B_X + c, 6, (char)CH_LIGHT, C_DGRAY);
    }
    vga_flush();
}

void arm_render_duel_lane(int lane, u32 step, int pid)
{
    char buf[16];
    int p = 0;
    int x = lane ? DUEL_B_X : DUEL_A_X;
    u8 color = lane ? C_MAG : C_CYAN;
    int hx;

    vga_text(x, 4, lane ? "TASK B" : "TASK A", color);
    vga_text(x + 8, 4, "PID", 0x18);
    p = fmt_dec(buf, (u32)pid);
    buf[p] = 0;
    vga_text(x + 12, 4, buf, 0x1E);
    vga_text(x + 16, 4, "< CPU BOUND", 0x18);

    vga_text(x, 5, "STEP", 0x18);
    p = 0;
    p += fmt_dec(buf, step);
    buf[p] = 0;
    vga_text(x + 5, 5, buf, 0x1F);

    duel_bar(x, 6, step, color);

    hx = x + (int)(step % (u32)DUEL_W);
    vga_cell(hx, 9, (char)CH_FULL, color);
    vga_cell(x + (int)((step + 1u) % (u32)DUEL_W), 9, (char)CH_LIGHT, 0x08);
}

void arm_render_duel_status(u32 a_steps, u32 b_steps, const u8 *tape, u32 tape_head)
{
    char buf[20];
    int p, c, r;
    u32 start = tape_head > (u32)DUEL_TAPE ? tape_head - (u32)DUEL_TAPE : 0;

    for (r = 12; r <= 14; r++) {
        for (c = 0; c < (int)DUEL_TAPE; c++) {
            u32 seq = start + (u32)c;
            u8 v = seq < tape_head ? tape[seq % (u32)DUEL_TAPE] : 0;
            u8 attr = v == 1 ? C_CYAN : (v == 2 ? C_MAG : 0x08);
            vga_cell(c, r, v ? (char)CH_FULL : ' ', attr);
        }
    }

    p = 0;
    p += fmt_dec(buf, task_preempt_total());
    buf[p] = 0;
    vga_text(18, 16, buf, 0x1E);

    p = 0;
    p += fmt_dec(buf, a_steps);
    buf[p] = 0;
    vga_text(10, 17, buf, 0x1F);

    p = 0;
    p += fmt_dec(buf, b_steps);
    buf[p] = 0;
    vga_text(49, 17, buf, 0x1F);
}

void arm_render_cpu_init(void)
{
    vga_clear(0x00);
    vga_flush();
}

/*
 * CPU 时间统计全屏。所有数字来自 task_snapshot()：
 * priority 是创建时写入的优先级，weight = priority + 1；
 * cpu_ticks 是 PIT/IRQ0 实际落在该任务上的 tick 数。
 */
void arm_render_cpu_frame(void)
{
    char buf[24];
    char name[10];
    struct task_info info;
    int slot, row = 5, i, p;
    u32 total = task_cpu_total();
    u32 sleep_blocks, sleep_wakes, timeouts;
    u32 io_blocks, io_wakes, io_timeouts;
    u32 wq_blocks, wq_one, wq_all, wq_depth;
    u32 idle_hlt;
    u32 quantum, slice_expire;
    u32 eintr;

    vga_clear(0x00);
    vga_text(16, 0, "AEVUM / CPU TIME ACCOUNTING", 0x1F);
    vga_text(5, 2, "PIT 100Hz / PRIORITY WEIGHTED FAIR SCHEDULER", 0x1B);
    vga_text(2, 4, "PID TASK      PRI WT STATE CPU% TICKS    SHARE", 0x0F);

    for (slot = 0; slot < TASK_MAX && row < 12; slot++) {
        u32 pct, n;
        if (!task_snapshot(slot, &info)) continue;

        for (i = 0; i < 8 && info.comm[i]; i++) name[i] = info.comm[i];
        if (info.comm[i]) name[i++] = '>';
        name[i] = 0;

        p = 0;
        p += fmt_dec(buf, (u32)info.pid);
        buf[p] = 0;
        vga_text(2, row, buf, C_GRAY);
        vga_text(6, row, name, task_pid_color((u32)info.pid));

        p = 0;
        p += fmt_dec(buf, (u32)info.priority);
        buf[p] = 0;
        vga_text(17, row, buf, C_YELLOW);

        p = 0;
        p += fmt_dec(buf, (u32)(info.priority + 1));
        buf[p] = 0;
        vga_text(21, row, buf, C_YELLOW);

        vga_cell(25, row, task_state_char(info.state),
                 info.state == TASK_RUNNING ? C_GREEN : C_GRAY);

        pct = total ? (info.cpu_ticks * 100u + total / 2u) / total : 0;
        if (pct > 100u) pct = 100u;
        p = 0;
        p += fmt_dec(buf, pct);
        buf[p] = 0;
        vga_text(31, row, buf, C_WHITE);
        vga_cell(34, row, '%', C_GRAY);

        p = 0;
        p += fmt_dec(buf, info.cpu_ticks);
        buf[p] = 0;
        vga_text(37, row, buf, C_WHITE);

        n = pct * 30u / 100u;
        for (i = 0; i < 30; i++)
            vga_cell(48 + i, row, i < (int)n ? (char)CH_FULL : (char)CH_LIGHT,
                     task_pid_color((u32)info.pid));
        row++;
    }

    p = 0;
    p += fmt_dec(buf, total);
    buf[p] = 0;
    vga_text(26, 18, "PIT TICKS", C_GRAY);
    vga_text(36, 18, buf, C_WHITE);

    p = 0;
    p += fmt_dec(buf, task_switch_total());
    buf[p] = 0;
    vga_text(26, 19, "SWITCHES", C_GRAY);
    vga_text(36, 19, buf, C_WHITE);

    task_slice_stats(&quantum, &slice_expire, 0);
    vga_text(52, 19, "QUANTUM", C_GRAY);
    p = 0;
    p += fmt_dec(buf, quantum);
    buf[p] = 0;
    vga_text(60, 19, buf, C_WHITE);
    vga_text(61, 19, "t", C_GRAY);
    vga_text(64, 19, "EXP", C_GRAY);
    p = 0;
    p += fmt_dec(buf, slice_expire);
    buf[p] = 0;
    vga_text(68, 19, buf, slice_expire ? C_GREEN : C_GRAY);

    p = 0;
    p += fmt_dec(buf, task_preempt_total());
    buf[p] = 0;
    vga_text(52, 18, "PREEMPTIONS", C_GRAY);
    vga_text(64, 18, buf, C_WHITE);

    vga_text(2, 20, "C = RETURN TO ARM", 0x1E);
    task_timer_stats(&sleep_blocks, &sleep_wakes, &timeouts);
    vga_text(22, 20, "SLEEP", C_GRAY);
    vga_text(28, 20, "b", C_GRAY);
    p = 0;
    p += fmt_dec(buf, sleep_blocks);
    buf[p] = 0;
    vga_text(30, 20, buf, C_WHITE);
    vga_text(34, 20, "w", C_GRAY);
    p = 0;
    p += fmt_dec(buf, sleep_wakes);
    buf[p] = 0;
    vga_text(36, 20, buf, C_WHITE);
    vga_text(40, 20, "tmo", C_GRAY);
    p = 0;
    p += fmt_dec(buf, timeouts);
    buf[p] = 0;
    vga_text(44, 20, buf, timeouts ? C_GREEN : C_GRAY);
    task_io_stats(&io_blocks, &io_wakes, &io_timeouts);
    vga_text(2, 21, "IO", C_GRAY);
    vga_text(5, 21, "b", C_GRAY);
    p = 0;
    p += fmt_dec(buf, io_blocks);
    buf[p] = 0;
    vga_text(7, 21, buf, C_WHITE);
    vga_text(10, 21, "w", C_GRAY);
    p = 0;
    p += fmt_dec(buf, io_wakes);
    buf[p] = 0;
    vga_text(12, 21, buf, C_WHITE);
    vga_text(15, 21, "tmo", C_GRAY);
    p = 0;
    p += fmt_dec(buf, io_timeouts);
    buf[p] = 0;
    vga_text(19, 21, buf, io_timeouts ? C_GREEN : C_GRAY);
    idle_hlt = task_idle_hlt_total();
    vga_text(26, 21, "IDLE hlt", C_GRAY);
    p = 0;
    p += fmt_dec(buf, idle_hlt);
    buf[p] = 0;
    vga_text(35, 21, buf, idle_hlt ? C_GREEN : C_GRAY);
    vga_text(2, 22, "priority 0..3 -> weight 8..1; EINTR", 0x18);
    eintr = task_eintr_total();
    p = 0;
    p += fmt_dec(buf, eintr);
    buf[p] = 0;
    vga_text(39, 22, buf, eintr ? C_GREEN : C_GRAY);
    task_wq_stats(&wq_blocks, &wq_one, &wq_all, &wq_depth);
    vga_text(2, 23, "WQ blk", C_GRAY);
    p = 0;
    p += fmt_dec(buf, wq_blocks);
    buf[p] = 0;
    vga_text(9, 23, buf, C_WHITE);
    vga_text(12, 23, "one", C_GRAY);
    p = 0;
    p += fmt_dec(buf, wq_one);
    buf[p] = 0;
    vga_text(16, 23, buf, wq_one ? C_GREEN : C_GRAY);
    vga_text(19, 23, "all", C_GRAY);
    p = 0;
    p += fmt_dec(buf, wq_all);
    buf[p] = 0;
    vga_text(23, 23, buf, wq_all ? C_GREEN : C_GRAY);
    vga_text(26, 23, "dep", C_GRAY);
    p = 0;
    p += fmt_dec(buf, wq_depth);
    buf[p] = 0;
    vga_text(30, 23, buf, wq_depth > 1 ? C_GREEN : C_GRAY);
    vga_text(2, 24, "QUANTUM/EXP = slice; WQ one=directed, all=broadcast; CPU%=ticks/total", 0x18);
}

void arm_render_rtq_init(void)
{
    vga_clear(0x00);
    vga_flush();
}

static void rtq_num(int x, int y, u32 value, u8 color)
{
    char buf[12];
    int n = fmt_dec(buf, value);
    buf[n] = 0;
    vga_text(x, y, buf, color);
}

static u8 rtq_lane_color(int prio)
{
    if (prio == 0) return C_GREEN;
    if (prio == 1) return C_CYAN;
    if (prio == 2) return C_YELLOW;
    return C_MAG;
}

static void rtq_box(int x, int y, int w, int h, u8 color)
{
    int c, r;

    for (c = x; c < x + w; c++) {
        vga_cell(c, y, (char)CH_H, color);
        vga_cell(c, y + h - 1, (char)CH_H, color);
    }
    for (r = y; r < y + h; r++) {
        vga_cell(x, r, (char)CH_V, color);
        vga_cell(x + w - 1, r, (char)CH_V, color);
    }
    vga_cell(x, y, (char)CH_TL, color);
    vga_cell(x + w - 1, y, (char)CH_TR, color);
    vga_cell(x, y + h - 1, (char)CH_BL, color);
    vga_cell(x + w - 1, y + h - 1, (char)CH_BR, color);
}

static void rtq_gauge(int x, int y, int w, u32 value, u32 max, u8 color)
{
    u32 n = max ? (value * (u32)w) / max : 0;
    int i;

    if (n > (u32)w) n = (u32)w;
    for (i = 0; i < w; i++)
        vga_cell(x + i, y, i < (int)n ? (char)CH_FULL : ' ',
                 i < (int)n ? color : 0x00);
}

/*
 * RT 队列全屏页：和 C/G/V 一样由主画面字母键进入，只读队列快照。
 * FIFO A/B 由 app 层的两个 priority 1 动态 kthread 写入，页面只显示结果。
 */
void arm_render_rtq_frame(u32 fifo_a, u32 fifo_b, u32 fifo_ok)
{
    char name[10];
    struct task_info info;
    int prio, i, row;
    u32 qerr = task_rt_queue_audit();
    u32 mask = task_rt_queue_mask();
    u32 total = task_rt_queue_total();

    vga_clear(0x00);
    vga_fill(0, 0, 79, 0, ' ', 0x1F);
    vga_text(24, 0, "AEVUM  //  REAL-TIME READY QUEUE", 0x1F);
    vga_text(61, 0, "RT DISPATCH", 0x1E);
    vga_text(8, 1, "BITMAP + 4 PRIORITY LANES  /  SAME-LEVEL FIFO  /  PREEMPTION", 0x1B);

    rtq_box(1, 2, 52, 8, C_CYAN);
    rtq_box(54, 2, 25, 8, C_MAG);

    vga_text(3, 3, "LANE", C_GRAY);
    vga_text(9, 3, "BIT", C_GRAY);
    vga_text(14, 3, "READY", C_GRAY);
    vga_text(22, 3, "HEAD", C_GRAY);
    vga_text(33, 3, "TAIL", C_GRAY);
    vga_text(43, 3, "LOAD", C_GRAY);

    for (prio = 0; prio <= TASK_PRIORITY_MAX; prio++) {
        int head = task_rt_queue_head(prio);
        int tail = task_rt_queue_tail(prio);
        u32 count = task_rt_queue_count(prio);
        u8 lane = rtq_lane_color(prio);

        row = 4 + prio;
        vga_text(3, row, "P", lane);
        rtq_num(4, row, (u32)prio, lane);
        vga_cell(10, row, (mask & (1u << prio)) ? '1' : '0',
                 (mask & (1u << prio)) ? C_GREEN : C_DGRAY);
        rtq_num(16, row, count, count ? C_WHITE : C_DGRAY);

        if (head >= 0 && task_snapshot(head, &info)) {
            for (i = 0; i < 8 && info.comm[i]; i++) name[i] = info.comm[i];
            if (info.comm[i]) name[i++] = '>';
            name[i] = 0;
            vga_text(22, row, name, task_pid_color((u32)info.pid));
        } else {
            vga_text(22, row, "-", C_DGRAY);
        }

        if (tail >= 0 && task_snapshot(tail, &info)) {
            for (i = 0; i < 8 && info.comm[i]; i++) name[i] = info.comm[i];
            if (info.comm[i]) name[i++] = '>';
            name[i] = 0;
            vga_text(33, row, name, task_pid_color((u32)info.pid));
        } else {
            vga_text(33, row, "-", C_DGRAY);
        }

        rtq_gauge(43, row, 8, count, 4, lane);
    }

    vga_text(3, 8, "QUEUED", C_GRAY);
    rtq_num(11, 8, total, C_WHITE);
    vga_text(18, 8, "MASK", C_GRAY);
    rtq_num(24, 8, mask, C_YELLOW);
    vga_text(31, 8, "AUDIT", C_GRAY);
    rtq_num(38, 8, qerr, qerr ? C_RED : C_GREEN);

    vga_text(56, 3, "FIFO BURST", C_WHITE);
    rtq_box(56, 4, 9, 4, C_CYAN);
    rtq_box(66, 4, 9, 4, C_MAG);
    vga_text(57, 5, "qrtA", C_CYAN);
    rtq_num(60, 6, fifo_a, C_WHITE);
    vga_text(67, 5, "qrtB", C_MAG);
    rtq_num(70, 6, fifo_b, C_WHITE);
    vga_text(76, 5, fifo_ok ? "OK" : "NO", fifo_ok ? C_GREEN : C_YELLOW);
    vga_text(56, 8, "A<B PRIO 1", fifo_ok ? C_GREEN : C_YELLOW);
    vga_text(68, 8, "DRAINED", C_GRAY);

    rtq_box(1, 10, 78, 5, C_CYAN);
    vga_text(3, 11, "DISPATCH TRACE", C_WHITE);
    vga_text(58, 11, "FIFO RAIL", C_GRAY);

    vga_text(3, 12, "[P1 A #", C_CYAN);
    rtq_num(10, 12, fifo_a, C_WHITE);
    vga_text(12, 12, "]", C_CYAN);
    vga_text(15, 12, "==>", C_YELLOW);
    vga_text(19, 12, "[P1 B #", C_MAG);
    rtq_num(26, 12, fifo_b, C_WHITE);
    vga_text(28, 12, "]", C_MAG);
    vga_text(31, 12, "==>", C_YELLOW);
    vga_text(35, 12, "[HEAD]", C_GREEN);
    vga_text(42, 12, "==>", C_YELLOW);
    vga_text(46, 12, "[RUN ]", C_GREEN);
    vga_text(53, 12, "==>", C_YELLOW);
    vga_text(57, 12, "[EXIT]", C_YELLOW);
    vga_text(65, 12, "QUEUE EMPTY", C_DGRAY);

    vga_text(3, 13, "ORDER", C_GRAY);
    vga_text(9, 13, "A", C_CYAN);
    rtq_num(11, 13, fifo_a, C_WHITE);
    vga_text(14, 13, "B", C_MAG);
    rtq_num(16, 13, fifo_b, C_WHITE);
    vga_text(20, 13, "FIFO", C_GRAY);
    vga_text(25, 13, fifo_ok ? "OK" : "WAIT", fifo_ok ? C_GREEN : C_YELLOW);
    vga_text(31, 13, "TAIL ENQUEUE -> HEAD DISPATCH -> EXIT", 0x18);

    rtq_box(1, 15, 78, 6, C_CYAN);
    vga_text(3, 16, "BITMAP", C_GRAY);
    for (prio = 3; prio >= 0; prio--) {
        int x = 12 + (3 - prio) * 9;
        u8 on = (mask & (1u << prio)) ? 1u : 0u;
        char pbuf[7];

        pbuf[0] = '[';
        pbuf[1] = 'P';
        pbuf[2] = (char)('0' + prio);
        pbuf[3] = ' ';
        pbuf[4] = on ? '1' : '0';
        pbuf[5] = ']';
        pbuf[6] = 0;
        vga_text(x, 16, pbuf, on ? rtq_lane_color(prio) : C_DGRAY);
    }
    vga_text(48, 16, "QUEUED", C_GRAY);
    rtq_num(56, 16, total, C_WHITE);
    vga_text(59, 16, "/16", C_GRAY);
    vga_text(64, 16, "ERR", C_GRAY);
    rtq_num(68, 16, qerr, qerr ? C_RED : C_GREEN);

    vga_text(3, 17, "SWITCHES", C_GRAY);
    rtq_gauge(13, 17, 45, task_switch_total(), 32768, C_CYAN);
    rtq_num(61, 17, task_switch_total(), C_WHITE);

    vga_text(3, 18, "FIFO SLOTS", C_GRAY);
    vga_text(14, 18, "[A 1]", C_CYAN);
    vga_text(20, 18, "==>", C_YELLOW);
    vga_text(24, 18, "[B 2]", C_MAG);
    vga_text(30, 18, "==>", C_YELLOW);
    vga_text(34, 18, "[END]", C_DGRAY);
    vga_text(41, 18, "A BEFORE B", fifo_ok ? C_GREEN : C_YELLOW);

    vga_text(2, 19, "TIMER", C_GRAY);
    vga_text(9, 19, interrupt_rt_timer_is_lapic() ? "LAPIC 1MS / HPET REF" : "PIT 100HZ / NO LAPIC", interrupt_rt_timer_is_lapic() ? C_GREEN : C_YELLOW);
    vga_text(33, 19, "SERVO", C_GRAY);
    rtq_num(39, 19, interrupt_rt_servo_total(), C_CYAN);
    vga_text(47, 19, "HARD", C_GRAY);
    rtq_num(52, 19, interrupt_rt_hard_total(), C_RED);

    vga_text(2, 21, "P0..P3: 0 HIGHEST / 3 LOWEST; HIGHER PRIO WINS", C_GRAY);

    vga_fill(0, 22, 79, 22, ' ', 0x1E);
    vga_text(2, 22, "[ Q ]  RETURN TO ARM", 0x1E);
    vga_text(56, 22, "FIFO A<B", fifo_ok ? 0x1A : 0x1E);

    vga_text(2, 23, "SAME PRIO: TAIL ENQUEUE, HEAD DISPATCH; BURST = TWO P1 KTHREADS", C_GRAY);
    vga_text(2, 24, "PICK / MAX / AVG RT-BENCH ONLY; NORMAL BUILD SHOWS SWITCH PULSE", C_GRAY);
    vga_flush();
}

void arm_render_frame(const struct arm_status *s)
{
    char buf[40];
    int c, r, i, p;
    u8 sc, sp_max;
    struct arm_vec2 tip;
    i32 vx_mm_s = 0, vy_mm_s = 0;

    tip = s->points.p[s->n];
    {
        i32 vx = 0, vy = 0;
        for (i = 0; i < s->n; i++) {
            i32 rx = tip.x - s->points.p[i].x;
            i32 ry = tip.y - s->points.p[i].y;
            vx += fx_mul(s->dq[i], -ry);
            vy += fx_mul(s->dq[i], rx);
        }
        vx_mm_s = vx * 1000;
        vy_mm_s = vy * 1000;
    }

    vga_clear(C_DGRAY);

    vga_text(1, 0, " N-DOF Planar Arm | PIT 100Hz | P/B/T/C/F/G/V/X/Y/K ", C_WHITE);
    p = 0;
    p += fmt_dec(buf, (u32)s->frame_no);
    buf[p] = 0;
    vga_text(62, 0, buf, C_GRAY);

    for (c = CV_X - 1; c <= CV_X + CV_W; c++) {
        vga_cell(c, CV_Y - 1, (char)CH_H, C_GRAY);
        vga_cell(c, CV_Y + CV_H, (char)CH_H, C_GRAY);
    }
    for (r = CV_Y - 1; r <= CV_Y + CV_H; r++) {
        vga_cell(CV_X - 1, r, (char)CH_V, C_GRAY);
        vga_cell(CV_X + CV_W, r, (char)CH_V, C_GRAY);
    }
    vga_cell(CV_X - 1, CV_Y - 1, (char)CH_TL, C_GRAY);
    vga_cell(CV_X + CV_W, CV_Y - 1, (char)CH_TR, C_GRAY);
    vga_cell(CV_X - 1, CV_Y + CV_H, (char)CH_BL, C_GRAY);
    vga_cell(CV_X + CV_W, CV_Y + CV_H, (char)CH_BR, C_GRAY);

    /* 主画面也给出 base 参考系：横轴左右，纵轴上下。 */
    for (c = CV_X; c <= CV_X + CV_W - 1; c++) vga_cell(c, wy(0), '.', C_GRAY);
    for (r = CV_Y; r <= CV_Y + CV_H - 1; r++) vga_cell(wx(0), r, '.', C_GRAY);

    draw_ghost_pose();

    for (i = 0; i < TRACE_N; i++) {
        u16 t = trace[i];
        if (t) vga_cell(t & 0xFF, (int)(t >> 8), '*', C_GRAY);
    }

    vga_cell(wx(s->target.x), wy(s->target.y), 'o', C_YELLOW);

    {
        int c0 = wx(s->points.p[0].x), r0 = wy(s->points.p[0].y);
        int c1, r1;
        for (i = 0; i < s->n; i++) {
            c1 = wx(s->points.p[i + 1].x);
            r1 = wy(s->points.p[i + 1].y);
            vga_line(c0, r0, c1, r1, (char)CH_FULL,
                     (i == s->n - 1) ? C_MAG : C_CYAN);
            if (i < s->n - 1) vga_cell(c1, r1, 'O', C_WHITE);
            c0 = c1;
            r0 = r1;
        }
        vga_cell(wx(s->points.p[0].x), wy(s->points.p[0].y), '#', C_WHITE);
        if (s->n > 0)
            vga_cell(wx(s->points.p[s->n].x), wy(s->points.p[s->n].y), '@', C_GREEN);
    }


    /* 四个方向的边界标记。 */
    vga_cell(CV_X - 1, wy(0), 'L', C_WHITE);
    vga_cell(CV_X + CV_W, wy(0), 'R', C_WHITE);
    vga_cell(wx(0), CV_Y - 1, 'U', C_WHITE);
    vga_cell(wx(0), CV_Y + CV_H - 1, 'D', C_WHITE);

    /* 关节旁的短标签：J1^ / J2> 表示该关节当前主要推动方向。 */
    for (i = 0; i < s->n; i++) {
        i32 rx = tip.x - s->points.p[i].x;
        i32 ry = tip.y - s->points.p[i].y;
        i32 tx = -ry;
        i32 ty = rx;
        char dir;
        int lc = wx(s->points.p[i].x) + 1;
        int lr = wy(s->points.p[i].y) - 1;

        if (s->dq[i] < 0) { tx = -tx; ty = -ty; }
        if ((tx < 0 ? -tx : tx) > (ty < 0 ? -ty : ty))
            dir = tx > 0 ? 'R' : 'L';
        else
            dir = ty > 0 ? 'U' : 'D';
        if (lc < CV_X) lc = CV_X;
        if (lc > CV_X + CV_W - 3) lc = CV_X + CV_W - 3;
        if (lr < CV_Y) lr = CV_Y;
        if (lr > CV_Y + CV_H - 1) lr = CV_Y + CV_H - 1;
        buf[0] = 'J';
        buf[1] = (char)('1' + i);
        buf[2] = dir;
        buf[3] = 0;
        vga_text(lc, lr, buf, C_DGRAY);
    }

    draw_minimap(s, vx_mm_s, vy_mm_s);

    ghost_age++;
    if (!ghost_valid || ghost_n != s->n || ghost_age >= 12) {
        ghost_points = s->points;
        ghost_n = s->n;
        ghost_valid = 1;
        ghost_age = 0;
    }

    {
        int X = 45;
        int per = (s->n + 1) / 2;
        int yy;
        if (per < 1) per = 1;
        vga_text(X, 1, "ARM STATUS", C_CYAN);
        vga_text(X + 22, 1, s->null_on ? "nullsp ON " : "nullsp OFF",
                 s->null_on ? C_GREEN : C_RED);
        for (c = X; c < X + 32; c++) vga_cell(c, 2, (char)CH_H, C_GRAY);

        for (i = 0; i < s->n; i++) {
            int cx = X + (i / per) * 16;
            int rr = 4 + (i % per);
            int L = 0, k;
            vga_cell(cx + 1, rr, 'q', C_GRAY);
            vga_cell(cx + 2, rr, (char)('1' + i), C_GRAY);
            fmt_fx(buf, fx_deg(s->q[i]), 1);
            while (buf[L]) L++;
            for (k = 0; k < 6 - L; k++) vga_cell(cx + 5 + k, rr, ' ', C_GRAY);
            vga_text(cx + 11 - L, rr, buf, C_WHITE);
            vga_cell(cx + 12, rr, (char)CH_DEG, C_GRAY);
        }

        yy = 9;
        vga_text(X, yy, "manipulability", C_GRAY);
        fmt_fx(buf, s->w_manip, 4);
        vga_text(X + 18, yy, buf, C_WHITE);

        yy = 10;
        vga_text(X, yy, "sigma min / max", C_GRAY);
        fmt_fx(buf, s->sigma_min, 4);
        if (s->sigma_min < 1311)      sc = C_RED;
        else if (s->sigma_min < 6554) sc = C_YELLOW;
        else                          sc = C_GREEN;
        vga_text(X + 18, yy, buf, sc);

        yy = 11;
        vga_text(X, yy, "speed damped", C_GRAY);
        {
            i32 sp = 0;
            for (i = 0; i < s->n; i++) {
                i32 a = s->dq[i] < 0 ? -s->dq[i] : s->dq[i];
                if (a > sp) sp = a;
            }
            fmt_fx(buf, sp, 2);
            sp_max = (sp > 157286) ? C_RED : (sp > 78643 ? C_YELLOW : C_GREEN);
            vga_text(X + 18, yy, buf, sp_max);
            vga_text(X + 25, yy, "rad/s", C_GRAY);
        }

        yy = 12;
        vga_text(X, yy, "limit margin", C_GRAY);
        fmt_fx(buf, s->limit_margin, 1);
        vga_text(X + 18, yy, buf, s->limit_margin < 10 ? C_RED : C_GREEN);
        vga_text(X + 22, yy, "deg", C_GRAY);

        yy = 13;
        vga_text(X, yy, "null share", C_GRAY);
        fmt_fx(buf, s->null_share, 3);
        vga_text(X + 18, yy, buf, C_WHITE);

        yy = 14;
        vga_text(X, yy, "target x / y", C_GRAY);
        fmt_fx(buf, s->target.x, 3);
        vga_text(X + 13, yy, buf, C_WHITE);
        fmt_fx(buf, s->target.y, 3);
        vga_text(X + 20, yy, buf, C_WHITE);

        yy = 15;
        vga_text(X, yy, "body joints", C_GRAY);
        buf[fmt_dec(buf, (u32)s->n)] = 0;
        vga_text(X + 18, yy, buf, C_WHITE);

        yy = 16;
        vga_text(X, yy, "mode", C_GRAY);
        vga_text(X + 18, yy, s->mode ? "MANUAL HOLD" : "AUTO",
                 s->mode ? C_YELLOW : C_GREEN);

        yy = 17;
        vga_text(X, yy, "frame", C_GRAY);
        buf[fmt_dec(buf, (u32)s->frame_no)] = 0;
        vga_text(X + 18, yy, buf, C_GRAY);

        task_panel();
    }

    task_timeline();
    vga_text(2, 22, "tip@ jointsO targeto path. base#  descriptor: 2..8 planar links", C_GRAY);
    {
        /* P8/P9：joined kping 的 tid/status，以及未 join korph 的自动回收数。 */
        vga_text(66, 22, "kt", C_GRAY);
        p = 0;
        p += fmt_dec(buf, arm_app_kthread_tid());
        buf[p] = 0;
        vga_text(69, 22, buf, C_WHITE);
        vga_text(72, 22, "s", C_GRAY);
        p = 0;
        p += fmt_dec(buf, arm_app_kthread_status());
        buf[p] = 0;
        vga_text(73, 22, buf, C_WHITE);
        vga_text(75, 22, "a", C_GRAY);
        p = 0;
        p += fmt_dec(buf, arm_app_kthread_auto());
        buf[p] = 0;
        vga_text(76, 22, buf, C_WHITE);
        vga_text(78, 22, arm_app_kthread_ok() ? "OK" : "..",
                 arm_app_kthread_ok() ? C_GREEN : C_RED);
    }
    vga_text(2, 23, "tip", C_GRAY);
    fmt_fx(buf, s->err_mm, 3);
    vga_text(6, 23, buf, C_WHITE);
    vga_text(12, 23, "mm", C_GRAY);
    vga_text(15, 23, "lim", C_GRAY);
    fmt_fx(buf, s->limit_margin, 1);
    vga_text(19, 23, buf, C_WHITE);
    vga_text(25, 23, "deg", C_GRAY);
    vga_text(29, 23, "null", C_GRAY);
    fmt_fx(buf, s->null_share, 3);
    vga_text(34, 23, buf, C_WHITE);
    {
        i32 wmin = (s->lap_smin_min == 0x7FFFFFFF) ? s->sigma_min : s->lap_smin_min;
        i32 wmx = s->lap_speed_max;
        if (wmin < 1966)
            vga_text(2, 24, ">>> smin collapsed: damping bounds the demand <<<", C_RED);
        else if (wmx > 196608)
            vga_text(2, 24, ">>> demand exceeds 3.00 rad/s motor limit; damping contains it <<<", C_YELLOW);
        else if (wmx > 78643)
            vga_text(2, 24, ">>> joint speed demand rises near full extension <<<", C_YELLOW);
        else
            vga_text(2, 24, "OK: joint speed demand inside 3.00 rad/s motor capability", C_GREEN);
    }
}

void arm_render_trace_push(const struct arm_status *s)
{
    i32 x = s->points.p[s->n].x;
    i32 y = s->points.p[s->n].y;

    trace[trace_i] = (u16)(wx(x) | (wy(y) << 8));
    trace_i = (trace_i + 1) % TRACE_N;
    mtrace[mtrace_i] = (u16)(mx(x) | (my(y) << 8));
    mtrace_i = (mtrace_i + 1) % TRACE_N;
}
