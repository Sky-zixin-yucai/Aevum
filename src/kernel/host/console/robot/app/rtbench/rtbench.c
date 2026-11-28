#ifdef AEVUM_RT_BENCH

#include "app/rtbench/rtbench.h"
#include "arch/x86/interrupt.h"
#include "arch/x86/lapic.h"
#include "arch/x86/tsc.h"
#include "config/memory.h"
#include "drivers/serial/uart.h"
#include "kernel/proc.h"
#include "kernel/task.h"
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
static u16 rt_lat_us[RT_PERIODS];

static u32 cycles_per_us;
static u32 calib_ok;
static u32 lat_count;
static u32 lat_max_cycles;
static u32 jitter_max_cycles;
static u32 exec_max_cycles;
static u32 deadline_miss;
static u32 overrun;
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
    u32 now;

    do {
        now = tsc_read32();
    } while ((u32)(now - start) < work_cycles);
}

static u32 rt_cycles_to_us(u32 cycles)
{
    return cycles / cycles_per_us;
}

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
    out(" overrun=");
    out_u32(overrun);
    out(" budget_overrun=");
    out_u32(task_rt_budget_overrun_total());
    out(" throttle=");
    out_u32(task_rt_budget_throttle_total());
    out(" servo=");
    out_u32(interrupt_rt_servo_total());
    out(" hard=");
    out_u32(interrupt_rt_hard_total());
    out(" armn=");
    out_u32(interrupt_rt_arm_total());
    out(" gap_us=");
    out_u32(interrupt_rt_gap_max_cycles() / (cycles_per_us ? cycles_per_us : 1u));
    out(" fgap_us=");
    out_u32(interrupt_rt_first_gap_cycles() / (cycles_per_us ? cycles_per_us : 1u));
    out(" cut_us=");
    out_u32(interrupt_rt_cut_cycles() / (cycles_per_us ? cycles_per_us : 1u));
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
    if (calib_ok) (void)lapic_timer_recalibrate(cycles_per_us);
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

        if (!wake) {
            deadline_miss++;
        } else if ((u32)(end - wake) > deadline_cycles) {
            deadline_miss++;
        }
        if (exec > budget_cycles) overrun++;
    }

    rt_report();
    process_stop_all();
    for (;;) (void)task_sleep_ticks(1000);
}

void rtbench_start(void)
{
    const char *rc;

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
