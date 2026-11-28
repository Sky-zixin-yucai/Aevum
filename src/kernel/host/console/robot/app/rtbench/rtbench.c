#ifdef AEVUM_RT_BENCH

#include "app/rtbench/rtbench.h"
#include "arch/x86/interrupt.h"
#include "arch/x86/lapic.h"
#include "arch/x86/tsc.h"
#include "config/memory.h"
#include "drivers/serial/uart.h"
#include "kernel/proc.h"
#include "kernel/task.h"
#include "kernel/task/stack.h"
#include "kernel/rt_mutex.h"
#include "kernel/mm/address_space.h"
#include "lib/format.h"

extern u8 user_program_start[];
extern u8 user_program_end[];

#ifndef RT_BG_TASKS
#define RT_BG_TASKS 0
#endif

#ifndef RT_WORK_US
#define RT_WORK_US 1800u
#endif

#ifndef RT_BUDGET
#define RT_BUDGET 0
#endif

#ifndef RT_BUDGET_ONESHOT
#define RT_BUDGET_ONESHOT 1
#endif

#ifndef RT_QUEUE_TEST
#define RT_QUEUE_TEST 0
#endif

#ifndef RT_PI_TEST
#define RT_PI_TEST 0
#endif

/*
 * P13.6：0 无继承，1 等待者优先级继承，2 立即优先级天花板。
 * 保留 RT_PI_ENABLE 作为旧命令行兼容：0->mode 0，1->mode 1。
 */
#ifndef RT_PI_MODE
#ifdef RT_PI_ENABLE
#define RT_PI_MODE (RT_PI_ENABLE ? 1 : 0)
#else
#define RT_PI_MODE 1
#endif
#endif

#ifndef RT_PI_CEILING
#define RT_PI_CEILING 0
#endif

#ifndef RT_PI_CHAIN
#define RT_PI_CHAIN 0
#endif

#ifndef RT_PI_CHAIN_ENABLE
#define RT_PI_CHAIN_ENABLE 1
#endif

#if RT_BG_TASKS > 12
#error "RT_BG_TASKS must be 0..12 (PROC_MAX=12)"
#endif

#define RT_MAX_BG        12
#define RT_PERIODS       1000u
#define RT_PERIOD_US     10000u
#define RT_BUDGET_US     2000u
#define RT_DEADLINE_US   10000u

static struct task_struct *rt_control_task;
static struct task_struct *rt_idle_task;
#if RT_QUEUE_TEST
static struct task_struct *rt_low_task;
static struct task_struct *rt_fifo_a_task;
static struct task_struct *rt_fifo_b_task;
static u32 rt_low_ticks;
static u32 rt_fifo_seq;
static u32 rt_fifo_a_ord;
static u32 rt_fifo_b_ord;
static u32 rt_fifo_ok;
#endif
#if RT_BG_TASKS > 0
static struct process *rt_bg[RT_MAX_BG];
#endif
#if RT_PI_TEST
static struct rt_mutex pi_mutex;
static struct task_struct *pi_low_task;
static struct task_struct *pi_med_task;
static volatile u32 pi_go;
static volatile u32 pi_low_locked;
static volatile u32 pi_med_stop;
static volatile u32 pi_med_ticks;
static volatile u32 pi_lock_p;
static u32 pi_wait_cycles;
static int pi_rc;
static u32 pi_locks;
static u32 pi_waits;
static u32 pi_boosts;
static u32 pi_restores;
#endif
static u16 rt_lat_us[RT_PERIODS];

static u32 cycles_per_us;
static u32 calib_ok;
static u32 lat_count;
static u32 lat_max_cycles;
static u32 jitter_max_cycles;
static u32 exec_max_cycles;
static u32 deadline_miss;
static u32 exec_wall_over;
static u32 cut_jobs;
static u32 post_cut_sum_us;
static u32 post_cut_max_cycles;
static u32 after_cut_sum_us;
static u32 after_cut_max_cycles;
static u32 wake_miss;
static u32 sleep_fail;
static u32 late_count;

static void out(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    uart_write((const u8 *)s, n);
}

static void out_u32(u32 v)
{
    char buf[12];
    int n = fmt_dec(buf, v);
    uart_write((const u8 *)buf, (u32)n);
}

static void rt_idle(void)
{
    for (;;) {
        task_note_idle_hlt();
        __asm__ __volatile__("sti; hlt" ::: "memory");
    }
}

#if RT_QUEUE_TEST
/*
 * P13.1：优先级 1 的 RT 探针。它只在 priority 0 的控制任务睡眠时运行；
 * 如果高优先级 RT 没有抢占它，控制周期的 miss 会立刻暴露。
 */
static void rt_low(void)
{
    for (;;) {
        rt_low_ticks++;
        (void)task_sleep_ticks(1);
    }
}

/*
 * P13.1 FIFO 探针：两个同优先级 RT 任务按创建顺序入队。A 先记录序号并
 * 永久阻塞，B 再运行；如果链尾插入正确，A=1、B=2。两个任务只跑一次，
 * 不抢 rtlow 的 tick 计数。
 */
static void rt_fifo_a(void)
{
    rt_fifo_seq++;
    rt_fifo_a_ord = rt_fifo_seq;
    (void)task_block();
    for (;;) (void)task_sleep_ticks(1000);
}

static void rt_fifo_b(void)
{
    rt_fifo_seq++;
    rt_fifo_b_ord = rt_fifo_seq;
    (void)task_block();
    for (;;) (void)task_sleep_ticks(1000);
}
#endif

static u32 rt_period_ticks(void)
{
    u32 hz = interrupt_tick_hz();
    if (!hz) hz = 100u;
    return (RT_PERIOD_US * hz + 999999u) / 1000000u;
}

static void rt_calibrate(void)
{
    u32 hz = interrupt_tick_hz();
    u32 period_ticks = rt_period_ticks();
    u32 cal_ticks = period_ticks * 10u;
    u32 t0, t1, c0, c1, dt;

    if (!hz || !period_ticks) {
        cycles_per_us = 1u;
        calib_ok = 0u;
        return;
    }

    t0 = task_tick_total();
    c0 = tsc_read32();
    (void)task_sleep_ticks(cal_ticks);
    t1 = task_tick_total();
    c1 = tsc_read32();
    dt = t1 - t0;

    if (!dt) {
        cycles_per_us = 1u;
        calib_ok = 0u;
        return;
    }

    cycles_per_us = ((c1 - c0) / dt) / (1000000u / hz);
    if (!cycles_per_us) {
        cycles_per_us = 1u;
        calib_ok = 0u;
    } else {
        calib_ok = 1u;
    }
}

static void rt_work(u32 start, u32 work_cycles)
{
    u32 last = start;
    u32 ran = 0;
    u32 gap_limit = cycles_per_us ? cycles_per_us * 1000u : 0x100000u;

    do {
        u32 now = tsc_read32();
        u32 delta = now - last;

        /*
         * 硬截断会留下 8ms 级 throttle 空档；只累计正常的微秒级循环间隔，
         * 超过 1ms 的间隔当作挂起，不计入 CPU 工作量。这样 3ms 作业被
         * 2ms 截断后，恢复时还会补跑剩余 1ms，而不是被墙钟直接判完成。
         */
        if (delta <= gap_limit) ran += delta;
        last = now;
    } while (ran < work_cycles);
}

static u32 rt_cycles_to_us(u32 cycles)
{
    return cycles / cycles_per_us;
}

#if RT_PI_TEST
/*
 * P13.5/P13.6 优先级反转夹具：
 * - pilow（RT 3）先拿锁，记录拿锁瞬间的有效优先级，然后阻塞等 pi_go；
 * - pimed（RT 1）在 pi_go 后 CPU-bound 3 tick；
 * - rtctl（RT 0）拿锁时若没有继承/天花板，会被 pimed 拖到约 30ms；
 *   等待者 PI 或立即天花板生效时，pilow 先跑完 1.5ms 临界区再释放。
 */
static void rt_pi_low(void)
{
    u32 start;

    if (rt_mutex_lock(&pi_mutex) != 0) {
        pi_low_locked = 2;
        for (;;) (void)task_block();
    }
    pi_lock_p = (u32)task_priority_get(0);
    pi_low_locked = 1;
    while (!pi_go) (void)task_block();

    start = tsc_read32();
    rt_work(start, cycles_per_us * 1500u);
    (void)rt_mutex_unlock(&pi_mutex);
    for (;;) (void)task_block();
}

static void rt_pi_med(void)
{
    u32 start = task_tick_total();

    while (!pi_med_stop && (u32)(task_tick_total() - start) < 3u)
        __asm__ __volatile__("pause" ::: "memory");
    pi_med_ticks = task_tick_total() - start;
    for (;;) (void)task_block();
}

static void rt_pi_run(void)
{
    const char *rc;
    u32 wait_start;
    int low_pid;

    rt_mutex_init(&pi_mutex, RT_PI_MODE == 1,
                  RT_PI_MODE == 2 ? RT_PI_CEILING : RT_MUTEX_NO_CEILING);
    pi_go = 0;
    pi_low_locked = 0;
    pi_med_stop = 0;
    pi_med_ticks = 0;
    pi_lock_p = 0;

    rc = task_create(3, "pilow", rt_pi_low, AEVUM_STACK_PROBE, &pi_low_task);
    if (rc) {
        out("PI_STAT create_low=");
        out(rc);
        out("\n");
        return;
    }
    out("PI_STEP low_created\n");
    task_set_rt(pi_low_task);
    low_pid = task_pid_of(pi_low_task);

    (void)task_sleep_ticks(2);
    out("PI_STEP low_wake locked=");
    out_u32(pi_low_locked);
    out("\n");
    if (!pi_low_locked) {
        out("PI_STAT low_lock=0 mode=");
        out_u32(RT_PI_MODE);
        out("\n");
        return;
    }

    rc = task_create(1, "pimed", rt_pi_med, AEVUM_STACK_DUEL_B, &pi_med_task);
    if (rc) {
        out("PI_STAT create_med=");
        out(rc);
        out("\n");
        return;
    }
    out("PI_STEP med_created\n");
    task_set_rt(pi_med_task);

    pi_go = 1;
    (void)task_wake(low_pid);
    out("PI_STEP low_unblocked\n");
    wait_start = tsc_read32();
    pi_rc = rt_mutex_lock(&pi_mutex);
    out("PI_STEP locked\n");
    pi_wait_cycles = tsc_read32() - wait_start;
    pi_med_stop = 1;

    rt_mutex_stats(&pi_mutex, &pi_locks, &pi_waits, &pi_boosts, &pi_restores);
    out("PI_STAT mode=");
    out_u32(RT_PI_MODE);
    out(" pi=");
    out_u32(RT_PI_MODE == 1 ? 1u : 0u);
    out(" rc=");
    out_u32(pi_rc < 0 ? (u32)(-pi_rc) : 0u);
    out(" wait_us=");
    out_u32(rt_cycles_to_us(pi_wait_cycles));
    out(" low_lock=");
    out_u32(pi_low_locked);
    out(" lock_p=");
    out_u32(pi_lock_p);
    out(" low_p=");
    out_u32((u32)task_priority_get(low_pid));
    out(" med_ticks=");
    out_u32(pi_med_ticks);
    out(" locks=");
    out_u32(pi_locks);
    out(" waits=");
    out_u32(pi_waits);
    out(" boost=");
    out_u32(pi_boosts);
    out(" restore=");
    out_u32(pi_restores);
    out("\n");
}
#endif

#if RT_PI_CHAIN
/*
 * P13.7 链式夹具：
 *   pimed(RT2) 拿 B 后阻塞在 chain_go；
 *   pilow(RT3) 拿 A 后阻塞在 B；
 *   pburn(RT1) CPU-bound，干扰 pimed；
 *   pchainhi(RT0) 等 A。
 * 开启 chain 时，pchainhi -> pilow(A) -> pimed(B) 传播，pimed 先跑；
 * 关闭 chain 时只提升 pilow，pburn 先跑满 3 tick。
 */
static struct rt_mutex chain_a_mutex;
static struct rt_mutex chain_b_mutex;
static struct task_struct *chain_low_task;
static struct task_struct *chain_med_task;
static struct task_struct *chain_burn_task;
static struct task_struct *chain_high_task;

static volatile u32 chain_go;
static volatile u32 chain_high_go;
static volatile u32 chain_burn_stop;
static volatile u32 chain_a_locked;
static volatile u32 chain_b_locked;
static volatile u32 chain_low_attempt;
static volatile u32 chain_burn_ticks;
static volatile u32 chain_med_run_p;
static volatile u32 chain_low_run_p;
static volatile u32 chain_med_p_after;
static volatile u32 chain_low_p_after;
static volatile u32 chain_high_done;
static u32 chain_wait_cycles;
static int chain_rc;
static int chain_high_rc;

static void rt_chain_med(void)
{
    u32 start;

    if (rt_mutex_lock(&chain_b_mutex) != 0) {
        chain_b_locked = 2;
        for (;;) task_block();
    }
    chain_b_locked = 1;
    while (!chain_go) task_block();

    chain_med_run_p = (u32)task_priority_get(0);
    start = tsc_read32();
    rt_work(start, cycles_per_us * 1500u);
    (void)rt_mutex_unlock(&chain_b_mutex);
    chain_med_p_after = (u32)task_priority_get(0);
    for (;;) task_block();
}

static void rt_chain_low(void)
{
    u32 start;

    if (rt_mutex_lock(&chain_a_mutex) != 0) {
        chain_a_locked = 2;
        for (;;) task_block();
    }
    chain_a_locked = 1;
    chain_low_attempt = 1;

    if (rt_mutex_lock(&chain_b_mutex) != 0) {
        chain_b_locked = 3;
        for (;;) task_block();
    }
    chain_low_run_p = (u32)task_priority_get(0);
    start = tsc_read32();
    rt_work(start, cycles_per_us * 1500u);
    (void)rt_mutex_unlock(&chain_b_mutex);
    (void)rt_mutex_unlock(&chain_a_mutex);
    chain_low_p_after = (u32)task_priority_get(0);
    for (;;) task_block();
}

static void rt_chain_burn(void)
{
    u32 start = task_tick_total();

    while (!chain_burn_stop && task_tick_total() - start < 3u)
        __asm__ __volatile__("pause");
    chain_burn_ticks = task_tick_total() - start;
    for (;;) task_block();
}

static void rt_chain_high(void)
{
    u32 start;

    while (!chain_high_go) task_block();
    start = tsc_read32();
    chain_rc = rt_mutex_lock(&chain_a_mutex);
    chain_wait_cycles = tsc_read32() - start;
    if (chain_rc == 0)
        chain_high_rc = rt_mutex_unlock(&chain_a_mutex);
    chain_high_done = 1;
    for (;;) task_block();
}

static void rt_pi_chain_run(void)
{
    u32 i;
    u32 a_locks, a_waits, a_boosts, a_restores;
    u32 b_locks, b_waits, b_boosts, b_restores;
    const char *cr;
    u32 med_pid;
    u32 med_stack, low_stack, high_stack, burn_stack;

    med_stack = kernel_stack_alloc();
    low_stack = kernel_stack_alloc();
    high_stack = kernel_stack_alloc();
    burn_stack = kernel_stack_alloc();
    if (!med_stack || !low_stack || !high_stack || !burn_stack) {
        out("PI_CHAIN_STAT stack=0\n");
        return;
    }

    rt_mutex_init(&chain_a_mutex, 1, RT_MUTEX_NO_CEILING);
    rt_mutex_init(&chain_b_mutex, 1, RT_MUTEX_NO_CEILING);
    rt_mutex_set_chain(&chain_a_mutex, RT_PI_CHAIN_ENABLE);
    rt_mutex_set_chain(&chain_b_mutex, RT_PI_CHAIN_ENABLE);

    chain_go = 0;
    chain_high_go = 0;
    chain_burn_stop = 0;
    chain_a_locked = 0;
    chain_b_locked = 0;
    chain_low_attempt = 0;
    chain_burn_ticks = 0;
    chain_med_run_p = 9;
    chain_low_run_p = 9;
    chain_med_p_after = 9;
    chain_low_p_after = 9;
    chain_high_done = 0;
    chain_rc = 0;
    chain_high_rc = 0;

    cr = task_create(2, "pimed", rt_chain_med, med_stack,
                     &chain_med_task);
    if (cr) {
        out("PI_CHAIN_STAT create_med=");
        out(cr);
        out("\n");
        return;
    }
    task_set_rt(chain_med_task);
    med_pid = task_pid_of(chain_med_task);
    (void)task_sleep_ticks(2);
    out("PI_CHAIN_STEP b_locked=");
    out_u32(chain_b_locked);
    out("\n");
    if (chain_b_locked != 1) {
        out("PI_CHAIN_STAT b_lock=0\n");
        return;
    }

    cr = task_create(3, "pilow", rt_chain_low, low_stack,
                     &chain_low_task);
    if (cr) {
        out("PI_CHAIN_STAT create_low=");
        out(cr);
        out("\n");
        return;
    }
    task_set_rt(chain_low_task);
    (void)task_sleep_ticks(2);
    out("PI_CHAIN_STEP a_locked=");
    out_u32(chain_a_locked);
    out(" low_attempt=");
    out_u32(chain_low_attempt);
    out("\n");
    if (chain_a_locked != 1 || chain_low_attempt != 1) {
        out("PI_CHAIN_STAT a_lock=0\n");
        return;
    }

    cr = task_create(0, "pchainhi", rt_chain_high, high_stack,
                     &chain_high_task);
    if (cr) {
        out("PI_CHAIN_STAT create_high=");
        out(cr);
        out("\n");
        return;
    }
    task_set_rt(chain_high_task);
    (void)task_sleep_ticks(1);

    cr = task_create(1, "pburn", rt_chain_burn, burn_stack,
                     &chain_burn_task);
    if (cr) {
        out("PI_CHAIN_STAT create_burn=");
        out(cr);
        out("\n");
        return;
    }
    task_set_rt(chain_burn_task);
    (void)task_sleep_ticks(1);

    /* pchainhi 已就绪但尚未放行；先让 pburn 占住 1 tick。 */
    chain_go = 1;
    (void)task_wake(med_pid);
    chain_high_go = 1;
    (void)task_wake(task_pid_of(chain_high_task));
    for (i = 0; i < 40u && !chain_high_done; i++)
        (void)task_sleep_ticks(1);
    chain_burn_stop = 1;

    rt_mutex_stats(&chain_a_mutex, &a_locks, &a_waits, &a_boosts, &a_restores);
    rt_mutex_stats(&chain_b_mutex, &b_locks, &b_waits, &b_boosts, &b_restores);
    out("PI_CHAIN_STAT chain=");
    out_u32(RT_PI_CHAIN_ENABLE);
    out(" rc=");
    out_u32(chain_rc < 0 ? (u32)(-chain_rc) : 0u);
    out(" high_rc=");
    out_u32(chain_high_rc < 0 ? (u32)(-chain_high_rc) : 0u);
    out(" done=");
    out_u32(chain_high_done);
    out(" wait_us=");
    out_u32(rt_cycles_to_us(chain_wait_cycles));
    out(" burn_ticks=");
    out_u32(chain_burn_ticks);
    out(" med_p=");
    out_u32(chain_med_run_p);
    out(" low_p=");
    out_u32(chain_low_run_p);
    out(" med_after=");
    out_u32(chain_med_p_after);
    out(" low_after=");
    out_u32(chain_low_p_after);
    out(" a_locks=");
    out_u32(a_locks);
    out(" b_locks=");
    out_u32(b_locks);
    out(" a_waits=");
    out_u32(a_waits);
    out(" b_waits=");
    out_u32(b_waits);
    out(" a_boost=");
    out_u32(a_boosts);
    out(" b_boost=");
    out_u32(b_boosts);
    out(" a_restore=");
    out_u32(a_restores);
    out(" b_restore=");
    out_u32(b_restores);
    out("\n");
}
#endif

static void rt_report(void)
{
    u32 i, j, v, n, p99;
    u32 switches = task_switch_total();

#if RT_QUEUE_TEST
    rt_fifo_ok = (rt_fifo_a_ord == 1u && rt_fifo_b_ord == 2u) ? 1u : 0u;
#endif

    for (i = 1; i < lat_count; i++) {
        v = rt_lat_us[i];
        j = i;
        while (j > 0 && rt_lat_us[j - 1] > v) {
            rt_lat_us[j] = rt_lat_us[j - 1];
            j--;
        }
        rt_lat_us[j] = (u16)v;
    }

    n = lat_count;
    if (!n) {
        p99 = 0;
    } else {
        u32 idx = (n * 99u) / 100u;
        if (idx >= n) idx = n - 1u;
        p99 = rt_lat_us[idx] * 10u;
    }

    out("RT_STAT bg=");
    out_u32(RT_BG_TASKS);
    out(" periods=");
    out_u32(RT_PERIODS);
    out(" lat_n=");
    out_u32(n);
    out(" lat_max_us=");
    out_u32(rt_cycles_to_us(lat_max_cycles));
    out(" lat_p99_us=");
    out_u32(p99);
    out(" jitter_max_us=");
    out_u32(rt_cycles_to_us(jitter_max_cycles));
    out(" exec_max_us=");
    out_u32(rt_cycles_to_us(exec_max_cycles));
    out(" miss=");
    out_u32(deadline_miss);
    out(" exec_wall_over=");
    out_u32(exec_wall_over);
    out(" budget_overrun=");
    out_u32(task_rt_budget_overrun_total());
    out(" throttle=");
    out_u32(task_rt_budget_throttle_total());
    out(" servo=");
    out_u32(interrupt_rt_servo_total());
    out(" hard=");
    out_u32(interrupt_rt_hard_total());
    out(" preempt=");
    out_u32(task_rt_budget_preempt_total());
    out(" armn=");
    out_u32(interrupt_rt_arm_total());
    out(" gap_us=");
    out_u32(interrupt_rt_gap_max_cycles() / (cycles_per_us ? cycles_per_us : 1u));
    out(" fgap_us=");
    out_u32(interrupt_rt_first_gap_cycles() / (cycles_per_us ? cycles_per_us : 1u));
    out(" cut_us=");
    out_u32(interrupt_rt_cut_cycles() / (cycles_per_us ? cycles_per_us : 1u));
    out(" cut_n=");
    out_u32(cut_jobs);
    out(" post_max_us=");
    out_u32(rt_cycles_to_us(post_cut_max_cycles));
    out(" post_avg_us=");
    out_u32(cut_jobs ? post_cut_sum_us / cut_jobs : 0u);
    out(" after_max_us=");
    out_u32(rt_cycles_to_us(after_cut_max_cycles));
    out(" after_avg_us=");
    out_u32(cut_jobs ? after_cut_sum_us / cut_jobs : 0u);
    out(" early=");
    out_u32(interrupt_rt_early_total());
    out(" ifoff=");
    out_u32(interrupt_rt_if_off_total());
    out(" lapic=");
    out_u32(interrupt_rt_timer_is_lapic());
    out(" wake_miss=");
    out_u32(wake_miss);
    out(" sleep_fail=");
    out_u32(sleep_fail);
    out(" late=");
    out_u32(late_count);
    out(" wresched=");
    out_u32(task_wake_resched_total());
    out(" switches=");
    out_u32(switches);
    out(" qerr=");
    out_u32(task_rt_queue_audit());
#if RT_QUEUE_TEST
    out(" low_ticks=");
    out_u32(rt_low_ticks);
    out(" fifo=");
    out_u32(rt_fifo_ok);
    out(" fifo_a=");
    out_u32(rt_fifo_a_ord);
    out(" fifo_b=");
    out_u32(rt_fifo_b_ord);
#endif
    out(" pick_max=");
    out_u32(task_pick_cycles_max());
    out(" pick_avg=");
    out_u32(task_pick_cycles_avg());
    out(" pick_n=");
    out_u32(task_pick_cycles_count());
    out(" cyc_us=");
    out_u32(cycles_per_us);
    out(" calib=");
    out_u32(calib_ok);
    out("\n");
}

static void rt_control(void)
{
#if RT_BG_TASKS > 0
    static const char *const names[RT_MAX_BG] = {
        "rtbg0", "rtbg1", "rtbg2", "rtbg3",
        "rtbg4", "rtbg5", "rtbg6", "rtbg7",
        "rtbg8", "rtbg9", "rtbg10", "rtbg11"
    };
    u32 code_size = (u32)user_program_end - (u32)user_program_start;
#endif
    u32 period_ticks = rt_period_ticks();
    u32 period_cycles;
    u32 budget_cycles;
    u32 work_cycles;
    u32 deadline_cycles;
    u32 prev_start = 0;
    u32 prev_valid = 0;
    u32 i;

    process_cleanup();
    out("CTL_STEP cleanup\n");

#if RT_BG_TASKS > 0
    for (i = 0; i < RT_BG_TASKS; i++) {
        rt_bg[i] = process_create_user((int)(i & 3u), names[i],
                                       user_program_start, code_size,
                                       AEVUM_USER_STACK_TOP);
        if (!rt_bg[i]) {
            out("RT_STAT create_failed lane=");
            out_u32(i);
            out("\n");
            for (;;) (void)task_sleep_ticks(1000);
        }
        /* mode 7：ring3 只自增计数，不 yield，由 PIT 抢占。 */
        address_space_set_mode(process_space(rt_bg[i]), 7u);
    }
#endif

    rt_calibrate();
    out("CTL_STEP calibrated\n");
    if (calib_ok) (void)lapic_timer_recalibrate(cycles_per_us);
#if RT_PI_TEST
    rt_pi_run();
#endif
#if RT_PI_CHAIN
    rt_pi_chain_run();
#endif
    period_cycles = cycles_per_us * RT_PERIOD_US;
    budget_cycles = cycles_per_us * RT_BUDGET_US;
    work_cycles = cycles_per_us * RT_WORK_US;
    deadline_cycles = cycles_per_us * RT_DEADLINE_US;
    if (!work_cycles) work_cycles = 1u;
    if (!budget_cycles) budget_cycles = 1u;

#if RT_BUDGET
    task_set_rt_budget(rt_control_task, budget_cycles, period_ticks);
#endif
    /* 选任务周期统计只覆盖正式周期，排除启动页和校准。 */
    task_pick_cycles_reset();

    out("RT_STAT start bg=");
    out_u32(RT_BG_TASKS);
    out(" hz=");
    out_u32(interrupt_tick_hz());
    out(" period_us=");
    out_u32(RT_PERIOD_US);
    out(" enforce=");
    out_u32(RT_BUDGET);
    out(" oneshot=");
    out_u32(RT_BUDGET_ONESHOT);
    out(" lapic=");
    out_u32(interrupt_rt_timer_is_lapic());
    out(" tmr_ticks_ms=");
    out_u32(interrupt_rt_ticks_per_ms());
    out(" budget_us=");
    out_u32(RT_BUDGET_US);
    out(" work_us=");
    out_u32(RT_WORK_US);
    out(" deadline_us=");
    out_u32(RT_DEADLINE_US);
    out(" periods=");
    out_u32(RT_PERIODS);
    out(" calib=");
    out_u32(calib_ok);
    out(" cyc_us=");
    out_u32(cycles_per_us);
    out(" one_ms_us=");
    out_u32(lapic_timer_test_1ms_cycles() / (cycles_per_us ? cycles_per_us : 1u));
    out("\n");

    for (i = 0; i < RT_PERIODS; i++) {
        u32 wake, start, end, latency, exec;
        u32 cut_before = interrupt_rt_hard_total();

        task_rt_probe_clear();
        if (task_sleep_ticks(period_ticks)) sleep_fail++;

        wake = task_rt_probe_wake_tsc();
        start = tsc_read32();

        if (wake) {
            u32 lat_units;
            latency = start - wake;
            if (latency > lat_max_cycles) lat_max_cycles = latency;
            /* 直方图/分位数组用 10us 为单位，u16 可覆盖 655ms。 */
            lat_units = rt_cycles_to_us(latency) / 10u;
            if (lat_units > 65535u) lat_units = 65535u;
            if (lat_count < RT_PERIODS) {
                rt_lat_us[lat_count] = (u16)lat_units;
                lat_count++;
            }
        } else {
            wake_miss++;
            latency = 0;
        }

        if (prev_valid && wake) {
            u32 delta = start - prev_start;
            u32 jitter = delta > period_cycles ?
                         delta - period_cycles : period_cycles - delta;
            if (jitter > jitter_max_cycles) jitter_max_cycles = jitter;
        }
        if (wake) {
            prev_start = start;
            prev_valid = 1;
        }

#if RT_BUDGET
        task_rt_budget_begin();
#if RT_BUDGET_ONESHOT
        /*
         * RT-2：把 100Hz 周期方波换成 2ms one-shot；预算 IRQ 处理完再装
         * 到下一个 10ms tick。rest_us 扣掉本次唤醒延迟，保持 tick 相位。
         */
        {
            u32 lat_us = wake ? rt_cycles_to_us(latency) : 0u;
            u32 rest_us = RT_PERIOD_US - RT_BUDGET_US;
            if (lat_us < rest_us) rest_us -= lat_us;
            else rest_us = 1000u;
            if (rest_us < 1000u) rest_us = 1000u;
            interrupt_arm_rt_budget(RT_BUDGET_US, rest_us);
        }
#endif
#endif

        rt_work(start, work_cycles);
        end = tsc_read32();
#if RT_BUDGET
        task_rt_budget_end();
#endif
        exec = end - start;
        if (exec > exec_max_cycles) exec_max_cycles = exec;
        if (interrupt_rt_hard_total() != cut_before) {
            u32 post = end - interrupt_rt_cut_tsc();
            u32 after = task_rt_budget_after_cut_cycles();
            u32 post_us = rt_cycles_to_us(post);
            u32 after_us = rt_cycles_to_us(after);

            cut_jobs++;
            post_cut_sum_us += post_us;
            if (post > post_cut_max_cycles) post_cut_max_cycles = post;
            after_cut_sum_us += after_us;
            if (after > after_cut_max_cycles) after_cut_max_cycles = after;
        }

        if (!wake) {
            deadline_miss++;
        } else if ((u32)(end - wake) > deadline_cycles) {
            deadline_miss++;
        }
        if (exec > budget_cycles) exec_wall_over++;
    }

    rt_report();
    process_stop_all();
    for (;;) (void)task_sleep_ticks(1000);
}

void rtbench_start(void)
{
    const char *rc;

    out("RT_BOOT P13.7\n");
    rc = task_create(3, "rtidle", rt_idle, AEVUM_STACK_IDLE, &rt_idle_task);
    if (rc) {
        out("RT_STAT task_create idle failed: ");
        out(rc);
        out("\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    task_set_idle_task(rt_idle_task);

    rc = task_create(0, "rtctl", rt_control, AEVUM_STACK_DUEL_A,
                     &rt_control_task);
    if (rc) {
        out("RT_STAT task_create control failed: ");
        out(rc);
        out("\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }

#if RT_QUEUE_TEST
    rc = task_create(1, "rtlow", rt_low, AEVUM_STACK_DUEL_B,
                     &rt_low_task);
    if (rc) {
        out("RT_STAT task_create low failed: ");
        out(rc);
        out("\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    task_set_rt(rt_low_task);

    rc = task_create(1, "rtfifoA", rt_fifo_a, AEVUM_STACK_PROBE,
                     &rt_fifo_a_task);
    if (rc) {
        out("RT_STAT task_create fifoA failed: ");
        out(rc);
        out("\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    task_set_rt(rt_fifo_a_task);

    rc = task_create(1, "rtfifoB", rt_fifo_b, AEVUM_STACK_P1,
                     &rt_fifo_b_task);
    if (rc) {
        out("RT_STAT task_create fifoB failed: ");
        out(rc);
        out("\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    task_set_rt(rt_fifo_b_task);
#endif

    task_rt_probe_set(rt_control_task);
    task_set_rt(rt_control_task);
    task_start(rt_control_task);
}

#endif
