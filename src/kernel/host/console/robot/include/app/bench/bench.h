#ifndef AEVUM_APP_BENCH_H
#define AEVUM_APP_BENCH_H

/* 只在 AEVUM_SCHED_BENCH 构建里使用：4 个 CPU-bound ring3 任务。 */
void bench_start(void) __attribute__((noreturn));

#endif
