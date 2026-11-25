#include "app/arm/render.h"
#include "app/arm/app.h"
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

#define WX0    (-55706)
#define WY0    (-58982)
#define WSPANX 111411
#define WSPANY 117965

#define TRACE_N 48

static u16 trace[TRACE_N];
static int trace_i;

void arm_render_init(void)
{
    int i;
    for (i = 0; i < TRACE_N; i++) trace[i] = 0;
    trace_i = 0;
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
    vga_text(2, 22, "priority 0..3 -> weight 1..4; smallest vruntime next", 0x18);
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

void arm_render_frame(const struct arm_status *s)
{
    char buf[40];
    int c, r, i, p;
    u8 sc, sp_max;

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

    for (c = CV_X; c <= CV_X + CV_W - 1; c++) vga_cell(c, wy(0), '.', C_GRAY);

    for (i = 0; i < TRACE_N; i++) {
        u16 t = trace[i];
        if (t) vga_cell(t & 0xFF, (int)(t >> 8), '.', C_BLUE);
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
    trace[trace_i] = (u16)(wx(s->points.p[s->n].x) | (wy(s->points.p[s->n].y) << 8));
    trace_i = (trace_i + 1) % TRACE_N;
}
