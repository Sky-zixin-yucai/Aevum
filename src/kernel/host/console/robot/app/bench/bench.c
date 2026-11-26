#include "app/bench/bench.h"
#include "config/memory.h"
#include "drivers/serial/uart.h"
#include "kernel/proc.h"
#include "kernel/task.h"
#include "kernel/mm/address_space.h"
#include "lib/format.h"

extern u8 user_program_start[];
extern u8 user_program_end[];

#ifndef BENCH_TASKS
#define BENCH_TASKS 4
#endif
#define BENCH_TICKS 500u   /* PIT 100 Hz -> 5 s */

static struct process *bench_proc[BENCH_TASKS];
static struct task_struct *bench_task;

#if BENCH_TASKS == 2
static const char *const bench_names[BENCH_TASKS] = {
    "benchA", "benchD"
};
static const int bench_prio[BENCH_TASKS] = { 0, 3 };
#elif BENCH_TASKS == 4
static const char *const bench_names[BENCH_TASKS] = {
    "benchA", "benchB", "benchC", "benchD"
};
static const int bench_prio[BENCH_TASKS] = { 0, 1, 2, 3 };
#else
#error "BENCH_TASKS must be 2 or 4"
#endif

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

static void out_share(u32 ticks, u32 total)
{
    u32 bp = total ? (u32)((ticks * 10000u) / total) : 0;
    out_u32(bp / 100u);
    out(".");
    out_u32((bp % 100u) / 10u);
    out_u32(bp % 10u);
    out("%");
}

static void bench_report(const char *tag)
{
    u32 ticks[BENCH_TASKS];
    u32 total = 0;
    int i;

    for (i = 0; i < BENCH_TASKS; i++) {
        struct task_info info;
        ticks[i] = 0;
        if (bench_proc[i] &&
            task_snapshot_process(bench_proc[i], &info)) {
            ticks[i] = info.cpu_ticks;
            total += ticks[i];
        }
    }

    out(tag);
    out(" total=");
    out_u32(total);
    for (i = 0; i < BENCH_TASKS; i++) {
        out(" p");
        out_u32((u32)i);
        out("=");
        out_u32(ticks[i]);
        out("(");
        out_share(ticks[i], total);
        out(")");
    }
    out("\n");
}

static void bench_entry(void)
{
    u32 code_size = (u32)user_program_end - (u32)user_program_start;
    int i;

    out("A0_BENCH start tasks=");
    out_u32(BENCH_TASKS);
    out(" slice=4 ticks=500\n");
    process_cleanup();

    for (i = 0; i < BENCH_TASKS; i++) {
        bench_proc[i] = process_create_user(bench_prio[i], bench_names[i],
                                            user_program_start, code_size,
                                            AEVUM_USER_STACK_TOP);
        if (!bench_proc[i]) {
            out("A0_BENCH create-failed lane=");
            out_u32((u32)i);
            out("\n");
            for (;;) task_sleep_ticks(1000);
        }
        /* 模式 7：ring3 只自增计数，不 yield，CPU-bound。 */
        address_space_set_mode(process_space(bench_proc[i]), 7u);
    }

    out("A0_BENCH created\n");
    (void)task_sleep_ticks(BENCH_TICKS);
    bench_report("A0_BENCH end");

    process_stop_all();
    out("A0_BENCH stopped\n");
    for (;;) (void)task_sleep_ticks(1000);
}

void bench_start(void)
{
    const char *rc;

    rc = task_create(0, "bench", bench_entry, AEVUM_STACK_DUEL_A, &bench_task);
    if (rc) {
        out("A0_BENCH task_create failed: ");
        out(rc);
        out("\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    }
    task_start(bench_task);
}
