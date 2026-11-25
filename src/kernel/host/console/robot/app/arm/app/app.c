#include "app/arm/app.h"
#include "app/arm/control.h"
#include "app/arm/render.h"
#include "app/fpu/demo.h"
#include "app/fpu/render.h"
#include "app/tree/demo.h"
#include "app/tree/render.h"
#include "app/user/demo.h"
#include "app/user/page.h"
#include "app/user/render.h"
#include "app/console/demo.h"
#include "app/console/render.h"
#include "app/pool/demo.h"
#include "app/pool/render.h"
#include "kernel/mm/address_space.h"
#include "kernel/task.h"
#include "kernel/task/family.h"
#include "kernel/task/stack.h"
#include "kernel/kthread.h"
#include "kernel/error.h"
#include "arch/x86/page_fault.h"
#include "drivers/video/vga.h"
#include "drivers/serial/uart.h"
#include "drivers/input/keyboard.h"
#include "drivers/console/console.h"
#include "config/memory.h"
#include "lib/format.h"

#define PROC_AUTO_TOGGLE 420
#define PROBE_FRAMES 6000
#define DUEL_TAPE 80

static struct task_struct *p_control;
static struct task_struct *p_render;
static struct task_struct *p_idle;
static int probe_live;
static struct task_struct *p_duel_a;
static struct task_struct *p_duel_b;
static volatile u32 duel_stop;
static volatile u32 duel_a;
static volatile u32 duel_b;
static volatile int duel_alive;
static volatile int duel_live;
static volatile int cpu_live;
static volatile int tree_live;
static volatile int user_live;
static volatile int console_live;
static volatile int console_exiting;
static volatile int pool_live;
static volatile int pool_exiting;
static volatile u8 duel_tape[DUEL_TAPE];
static volatile u32 duel_tape_head;
static u32 probe_runs;
static volatile int idle_pid;
static volatile u32 idle_block_req;
static volatile u32 idle_blocked;

/* P8 动态内核线程自测：先 join 一个 kping，再故意不 join 一个 korph。 */
static int kthread_demo_started;
static u32 kthread_demo_tid;
static u32 kthread_demo_status;
static u32 kthread_demo_ok;
static u32 kthread_demo_steps;
static u32 kthread_demo_stack_before;
static u32 kthread_demo_stack_after;
static u32 kthread_demo_auto_reaped;
static u32 kthread_demo_orphan_tid;
static u32 kthread_demo_orphan_steps;

static const struct aevum_error app_dep_template = {
    "arm_app", "task_create", "dependency", "arm app task assembly failed", 0
};

static const struct aevum_error app_state_template = {
    "arm_app", "task_yield", "state", "arm app task switch failed", 0
};

static void send_arm_status(const struct arm_status *st)
{
    i32 v[AEVUM_BODY_MAX_JOINTS * 3 + 10];
    int i, n = st->n;
    int base = n * 3;

    for (i = 0; i < n; i++) {
        v[i] = st->q[i];
        v[n + i] = st->dq[i];
        v[2 * n + i] = st->q_target[i];
    }
    v[base + 0] = st->target.x;
    v[base + 1] = st->target.y;
    v[base + 2] = st->err_mm;
    v[base + 3] = st->w_manip;
    v[base + 4] = st->sigma_min;
    v[base + 5] = st->sigma_max;
    v[base + 6] = st->null_share;
    v[base + 7] = st->limit_margin;
    v[base + 8] = st->mode;
    v[base + 9] = st->null_on;
    uart_status_frame(v, (u32)(base + 10));
}

static void kthread_demo_entry(void *arg)
{
    volatile u32 *steps = (volatile u32 *)arg;
    int i;

    for (i = 0; i < 192; i++) {
        (*steps)++;
        if (task_yield()) kthread_exit(1u);
    }
    kthread_exit(5u);
}

/* 故意不 join：退出后由调度器在 grace tick 后兜底回收。 */
static void kthread_orphan_entry(void *arg)
{
    volatile u32 *steps = (volatile u32 *)arg;
    int i;

    for (i = 0; i < 8; i++) {
        (*steps)++;
        if (task_yield()) kthread_exit(6u);
    }
    kthread_exit(6u);
}

static void kthread_demo_run_once(void)
{
    struct kthread_stats stats;
    u32 status = 0;
    u32 start_tick;
    int tid, orphan_tid;

    if (kthread_demo_started) return;
    kthread_demo_started = 1;

    kthread_demo_stack_before = (u32)kernel_stack_used();
    tid = kthread_create("kping", kthread_demo_entry,
                         (void *)&kthread_demo_steps, 0);
    if (tid < 0) {
        kthread_demo_ok = 0;
        return;
    }
    kthread_demo_tid = (u32)tid;

    if (kthread_join(tid, &status) != 0) {
        kthread_demo_ok = 0;
        return;
    }
    kthread_demo_status = status;

    /*
     * P9：故意不 join 的 kthread。它退出后先留 ZOMBIE，等过 grace
     * 由调度器回收；这里等 140 tick，确保兜底路径真的跑过一次。
     */
    orphan_tid = kthread_create("korph", kthread_orphan_entry,
                                (void *)&kthread_demo_orphan_steps, 0);
    if (orphan_tid < 0) {
        kthread_demo_ok = 0;
        return;
    }
    kthread_demo_orphan_tid = (u32)orphan_tid;

    start_tick = task_tick_total();
    while ((u32)(task_tick_total() - start_tick) < 140u) {
        if (task_yield()) {
            kthread_demo_ok = 0;
            return;
        }
    }

    kthread_demo_stack_after = (u32)kernel_stack_used();
    kthread_stats(&stats);
    kthread_demo_auto_reaped = stats.auto_reaped;
    kthread_demo_ok = (status == 5u &&
                       kthread_demo_stack_after == kthread_demo_stack_before &&
                       stats.join_blocks > 0 && stats.reaped == 2u &&
                       stats.auto_reaped == 1u && stats.live == 0u) ? 1u : 0u;
}

static void proc_control(void)
{
    struct uart_input in;
    u32 next_tick = task_tick_total();

    /* 先跑一次动态 kthread 自测；control 会在 join 上真正 BLOCKED。 */
    kthread_demo_run_once();
    /* 自测会阻塞若干 tick，重置控制节拍基准，避免启动时补跑一串控制步。 */
    next_tick = task_tick_total();

    for (;;) {
        while (uart_poll_input(&in)) {
            if (in.type == UART_CMD_RELEASE)
                arm_control_release_manual();
            else if (in.type == UART_CMD_BODY)
                arm_control_set_body(&in.body);
            else if (in.type == UART_CMD_JOINT)
                arm_control_set_joint_targets(in.q, in.n);
        }
        arm_control_step();

        /* 控制步按 PIT 100 Hz 走；不能再让调度器跑多快就积分多快。 */
        next_tick++;
        while ((i32)(task_tick_total() - next_tick) < 0) {
            if (task_yield()) {
                struct aevum_error e = app_state_template;
                e.field = "task_yield(control)";
                aevum_fail(&e);
            }
        }
    }
}

static void proc_render(void)
{
    int frames = 0;

    for (;;) {
        struct arm_status status;
        u8 key = keyboard_poll();

        /* P4.4 console 页是独立全屏模式；X 进入后所有非命令键进入 fd0 队列。 */
        if (key == 0x2D && !console_live && !duel_live && !cpu_live &&
            !probe_live && !fpu_demo_live() && !tree_live && !user_live) {
            const char *rc = console_demo_start();
            if (rc) {
                struct aevum_error e = app_dep_template;
                e.field = rc;
                e.at = "proc_render -> console_demo_start";
                aevum_fail(&e);
            }
            console_live = 1;
            console_exiting = 0;
            continue;
        }

        if (console_live) {
            const char *rc;

            if (key == 0x2D) {
                if (console_demo_alive()) {
                    console_exiting = 1;
                    console_demo_stop();
                } else {
                    console_demo_cleanup();
                    console_live = 0;
                    console_exiting = 0;
                    continue;
                }
            } else if (key) {
                console_input_push(key);
            }

            rc = console_demo_error();
            if (rc) {
                struct aevum_error e = app_dep_template;
                e.field = rc;
                e.at = "proc_render -> console_demo";
                aevum_fail(&e);
            }
            if (console_exiting && !console_demo_alive()) {
                console_demo_cleanup();
                console_live = 0;
                console_exiting = 0;
                continue;
            }
            console_render_frame();
            vga_flush();
            if (task_yield()) {
                struct aevum_error e = app_state_template;
                e.field = "task_yield(render/console)";
                aevum_fail(&e);
            }
            continue;
        }

        /* P5.3 压测页：S 进入；父进程 100 轮 fork/wait，子进程 exit(7)。 */
        if (key == 0x1F && !console_live && !duel_live && !cpu_live &&
            !probe_live && !fpu_demo_live() && !tree_live && !user_live &&
            !pool_live) {
            const char *rc = pool_demo_start(1);
            if (rc) {
                struct aevum_error e = app_dep_template;
                e.field = rc;
                e.at = "proc_render -> pool_demo_start(6)";
                aevum_fail(&e);
            }
            pool_live = 1;
            pool_exiting = 0;
            continue;
        }

        /* P5.1 池压力页：Y 进入；根进程 fork 8 个子进程，全部保持可调度。 */
        if (key == 0x15 && !console_live && !duel_live && !cpu_live &&
            !probe_live && !fpu_demo_live() && !tree_live && !user_live &&
            !pool_live) {
            const char *rc = pool_demo_start(0);
            if (rc) {
                struct aevum_error e = app_dep_template;
                e.field = rc;
                e.at = "proc_render -> pool_demo_start";
                aevum_fail(&e);
            }
            pool_live = 1;
            pool_exiting = 0;
            continue;
        }

        if (pool_live) {
            const char *rc;

            if (key == (pool_demo_is_stress() ? 0x1F : 0x15)) {
                if (pool_demo_alive()) {
                    pool_exiting = 1;
                    pool_demo_stop();
                } else {
                    pool_demo_cleanup();
                    pool_live = 0;
                    pool_exiting = 0;
                    continue;
                }
            }

            rc = pool_demo_error();
            if (rc) {
                struct aevum_error e = app_dep_template;
                e.field = rc;
                e.at = "proc_render -> pool_demo";
                aevum_fail(&e);
            }
            if (pool_exiting && !pool_demo_alive()) {
                pool_demo_cleanup();
                pool_live = 0;
                pool_exiting = 0;
                continue;
            }
            pool_render_frame();
            vga_flush();
            if (task_yield()) {
                struct aevum_error e = app_state_template;
                e.field = "task_yield(render/pool)";
                aevum_fail(&e);
            }
            continue;
        }

        if (key == 0x2E && !duel_live && !probe_live && !fpu_demo_live() && !tree_live && !user_live) { /* C：CPU 时间统计 */
            cpu_live = !cpu_live;
            if (cpu_live) arm_render_cpu_init();
        }

        if (key == 0x14 && !cpu_live && !fpu_demo_live() && !tree_live && !user_live) {    /* T：开始/停止抢占决斗 */
            if (duel_live) duel_stop = 1;
            else if (!probe_live) (void)arm_app_start_duel();
        }

        if (key == 0x21 && !duel_live && !cpu_live && !probe_live && !tree_live && !user_live) { /* F：FPU context lab */
            if (fpu_demo_live()) {
                fpu_demo_stop();
            } else {
                const char *rc = fpu_demo_start();
                if (rc) {
                    struct aevum_error e = app_dep_template;
                    e.field = rc;
                    e.at = "proc_render -> fpu_demo_start";
                    aevum_fail(&e);
                }
            }
        }

        if (key == 0x22 && !duel_live && !cpu_live && !probe_live && !fpu_demo_live() && !user_live) { /* G：进程家族树 */
            if (tree_live) {
                tree_demo_stop();
            } else {
                const char *rc = tree_demo_start();
                if (rc) {
                    struct aevum_error e = app_dep_template;
                    e.field = rc;
                    e.at = "proc_render -> tree_demo_start";
                    aevum_fail(&e);
                }
                tree_live = 1;
            }
        }

        if (key == 0x2F && !duel_live && !cpu_live && !probe_live && !fpu_demo_live() && !tree_live) { /* V：ring3 进程上下文 */
            if (user_live) {
                user_demo_stop();
            } else {
                const char *rc = user_demo_start();
                if (rc) {
                    struct aevum_error e = app_dep_template;
                    e.field = rc;
                    e.at = "proc_render -> user_demo_start";
                    aevum_fail(&e);
                }
                user_page_reset();
                user_live = 1;
            }
        }

        /* V 页的 TAB/数字/方向/PgUp/PgDn/Home/End 只改 UI 状态，不动进程。 */
        if (user_live) (void)user_page_key(key);

        if (key == 0x25 && !duel_live && !cpu_live && !probe_live && !fpu_demo_live() && !tree_live && !user_live) { /* K：内核实控 #PF panic 页 */
            page_fault_probe();
        }

        if (!duel_live && !cpu_live && !probe_live && !fpu_demo_live() && !tree_live && !user_live) {
            if (key == 0x39) arm_control_toggle_nullspace();
            if (key == 0x19) (void)arm_app_spawn_probe();
            if (key == 0x30) {
                if (idle_blocked) (void)task_wake(idle_pid);
                else idle_block_req = 1;
            }
        }

        /* ring3 进程上下文：地址空间由 mm 模块持有，render 只读。 */
        if (user_live) {
            const char *rc = user_demo_error();
            if (rc) {
                struct aevum_error e = app_dep_template;
                e.field = rc;
                e.at = "proc_render -> user_demo";
                aevum_fail(&e);
            }
            if (!user_demo_alive()) {
                user_live = 0;
            } else {
                user_render_frame();
                vga_flush();
                if (task_yield()) {
                    struct aevum_error e = app_state_template;
                    e.field = "task_yield(render/user)";
                    aevum_fail(&e);
                }
                continue;
            }
        }

        /* 进程家族树：family.c 负责关系，render 只读；G 停止后等任务退完再回机械臂。 */
        if (tree_live) {
            const char *rc = tree_demo_error();
            if (rc) {
                struct aevum_error e = app_dep_template;
                e.field = rc;
                e.at = "proc_render -> tree_demo";
                aevum_fail(&e);
            }
            if (!tree_demo_alive()) {
                tree_live = 0;
            } else {
                tree_render_frame();
                vga_flush();
                if (task_yield()) {
                    struct aevum_error e = app_state_template;
                    e.field = "task_yield(render/tree)";
                    aevum_fail(&e);
                }
                continue;
            }
        }

        /* 两个 CPU 密集型 lane 占用全屏；render 只刷新和观察。 */
        if (duel_live) {
            if (duel_alive == 0) {
                duel_live = 0;
            } else {
                arm_render_duel_status(duel_a, duel_b, (const u8 *)duel_tape, duel_tape_head);
                vga_flush();
                if (task_yield()) {
                    struct aevum_error e = app_state_template;
                    e.field = "task_yield(render/duel)";
                    aevum_fail(&e);
                }
                continue;
            }
        }

        /* FPU context lab：两条 lane 的 XMM0 必须各自回来。 */
        if (fpu_demo_live()) {
            fpu_render_frame();
            vga_flush();
            if (task_yield()) {
                struct aevum_error e = app_state_template;
                e.field = "task_yield(render/fpu)";
                aevum_fail(&e);
            }
            continue;
        }

        /* CPU 统计全屏：只观察，不重绘机械臂。 */
        if (cpu_live) {
            arm_render_cpu_frame();
            vga_flush();
            if (task_yield()) {
                struct aevum_error e = app_state_template;
                e.field = "task_yield(render/cpu)";
                aevum_fail(&e);
            }
            continue;
        }

        /* 探针占用全屏时，render 不再抢画，但仍在调度器里正常让出。 */
        if (probe_live) {
            if (task_yield()) {
                struct aevum_error e = app_state_template;
                e.field = "task_yield(render/probe-live)";
                aevum_fail(&e);
            }
            continue;
        }

        arm_control_status(&status);
        arm_render_frame(&status);
        if (idle_blocked)
            vga_text(2, 2, "BLOCKED: idle waiting task_wake()", 0x09);
        vga_flush();
        if ((frames & 3) == 0) {
            uart_scan();
            send_arm_status(&status);
        }
        vga_vsync();
        arm_render_trace_push(&status);

        if ((frames % PROC_AUTO_TOGGLE) == 0)
            arm_control_toggle_nullspace();
        frames++;

        if (task_yield()) {
            struct aevum_error e = app_state_template;
            e.field = "task_yield(render)";
            aevum_fail(&e);
        }
    }
}

static void proc_probe(void)
{
    char buf[16];
    int i, c, p, pid = task_current_pid();

    for (i = 0; i < PROBE_FRAMES; i++) {
        int life = ((PROBE_FRAMES - i) * 100) / PROBE_FRAMES;

        vga_clear(0x10);
        vga_text(23, 1, "AEVUM / PID PROBE", 0x1F);
        vga_text(18, 3, "LIVE PROCESS DESCRIPTOR", 0x1B);

        p = 0;
        p += fmt_dec(buf, (u32)pid);
        buf[p] = 0;
        vga_text(28, 5, "PID", 0x1B);
        vga_text(32, 5, buf, 0x1E);
        vga_text(35, 5, "< allocated by task.c", 0x1F);

        p = 0;
        p += fmt_dec(buf, probe_runs);
        buf[p] = 0;
        vga_text(62, 5, "RUN #", 0x1B);
        vga_text(68, 5, buf, 0x1E);

        vga_text(12, 7, "state RUNNING   stack 0xC000..0xD000", 0x1A);
        vga_text(12, 9, "kernel PID bitmap 1..31  /  no hard-coded 1,2,3", 0x18);

        for (c = 8; c < 72; c++)
            vga_cell(c, 11, (char)(c == 8 + ((i * 64) / PROBE_FRAMES) ? 0xDB : 0xB0), 0x1B);

        p = 0;
        p += fmt_dec(buf, (u32)life);
        buf[p] = 0;
        vga_text(12, 13, "life", 0x18);
        vga_text(17, 13, buf, 0x1E);
        vga_text(20, 13, "%", 0x18);

        vga_text(12, 15, "RUNNING  ->  ZOMBIE  ->  REAP  ->  PID REUSED", 0x1F);
        vga_text(12, 17, "press P after return: the same PID is issued again", 0x1E);
        vga_text(12, 19, "this is a real scheduled task, not a printed bitmap", 0x18);

        p = 0;
        p += fmt_dec(buf, task_preempt_total());
        buf[p] = 0;
        vga_text(12, 21, "PIT PREEMPTIONS", 0x18);
        vga_text(29, 21, buf, 0x1E);
        vga_text(34, 21, "timer IRQ0 switched this CPU-bound loop", 0x1B);
        vga_flush();
        vga_vsync();
    }

    probe_live = 0;
    task_exit();
}

/*
 * 两个 CPU 密集型任务：循环里没有 task_yield()。
 * 如果 PIT/IRQ0 不抢占，先跑的那个会把另一个彻底饿死。
 */
static void proc_duel(int lane)
{
    int pid = task_current_pid();
    u32 step = 0;
    u32 last_px = task_preempt_total();
    volatile u32 spin;

    for (;;) {
        if (duel_stop) break;
        step++;
        if (lane) duel_b = step;
        else duel_a = step;
        if (task_preempt_total() != last_px) {
            last_px = task_preempt_total();
            duel_tape[duel_tape_head % (u32)DUEL_TAPE] = (u8)(lane + 1);
            duel_tape_head++;
        }
        if ((step & 63u) == 0)
            arm_render_duel_lane(lane, step, pid);
        for (spin = 0; spin < 12u; spin++)
            __asm__ __volatile__("" ::: "memory");
    }

    arm_render_duel_lane(lane, step, pid);
    duel_alive--;
    task_exit();
}

static void proc_duel_a(void) { proc_duel(0); }
static void proc_duel_b(void) { proc_duel(1); }

static void proc_idle(void)
{
    idle_pid = task_current_pid();
    for (;;) {
        if (idle_block_req) {
            idle_block_req = 0;
            idle_blocked = 1;
            if (task_block()) idle_blocked = 0;
            idle_blocked = 0;
        }
        if (task_yield()) {
            struct aevum_error e = app_state_template;
            e.field = "task_yield(idle)";
            aevum_fail(&e);
        }
    }
}

const char *arm_app_spawn_probe(void)
{
    struct task_struct *probe;
    const char *rc;

    if (probe_live) return 0;
    rc = task_create(1, "probe", proc_probe, AEVUM_STACK_PROBE, &probe);
    if (!rc) {
        probe_live = 1;
        probe_runs++;
    }
    return rc;
}

const char *arm_app_start_duel(void)
{
    const char *rc;

    if (duel_live || probe_live) return 0;

    duel_stop = 0;
    duel_a = 0;
    duel_b = 0;
    duel_alive = 2;
    duel_live = 1;
    duel_tape_head = 0;
    arm_render_duel_init();

    rc = task_create(1, "spinA", proc_duel_a, AEVUM_STACK_DUEL_A, &p_duel_a);
    if (rc) {
        duel_stop = 1;
        duel_alive = 0;
        duel_live = 0;
        return rc;
    }

    rc = task_create(1, "spinB", proc_duel_b, AEVUM_STACK_DUEL_B, &p_duel_b);
    if (rc) {
        /* 第一个任务已经存在，让它看到 stop 后自己退出。 */
        duel_stop = 1;
        duel_alive = 1;
        return rc;
    }
    return 0;
}

const char *arm_app_init(void)
{
    const char *rc;

    arm_control_init();
    arm_render_init();

    rc = task_create(3, "control", proc_control, AEVUM_STACK_P1, &p_control);
    if (rc) return rc;

    rc = task_create(2, "render", proc_render, AEVUM_STACK_P2, &p_render);
    if (rc) return rc;

    rc = task_create(0, "idle", proc_idle, AEVUM_STACK_IDLE, &p_idle);
    if (rc) return rc;
    family_set_reaper(task_pid_of(p_idle));

    return 0;
}

void arm_app_start(void)
{
    if (!p_control || !p_render || !p_idle) {
        struct aevum_error e = app_dep_template;
        e.field = "p_control/p_render/p_idle";
        e.at = "arm_app_start";
        aevum_fail(&e);
    }

    task_start(p_control);
}

u32 arm_app_kthread_tid(void) { return kthread_demo_tid; }
u32 arm_app_kthread_status(void) { return kthread_demo_status; }
u32 arm_app_kthread_ok(void) { return kthread_demo_ok; }
u32 arm_app_kthread_auto(void) { return kthread_demo_auto_reaped; }
u32 arm_app_kthread_orphan_tid(void) { return kthread_demo_orphan_tid; }
u32 arm_app_kthread_steps(void) { return kthread_demo_steps; }
u32 arm_app_kthread_stack_before(void) { return kthread_demo_stack_before; }
u32 arm_app_kthread_stack_after(void) { return kthread_demo_stack_after; }
