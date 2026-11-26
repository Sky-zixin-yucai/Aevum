#ifndef AEVUM_ARCH_X86_TSC_H
#define AEVUM_ARCH_X86_TSC_H

#include "kernel/types.h"

/*
 * RT-0b：只读 TSC 低 32 位，用于相对 latency/jitter 采样。
 * 10ms 级周期内差值远小于 2^32，不需要 64 位除法；
 * QEMU 的 TSC 只作相对比较，硬实时最终要在真实硬件上复测。
 */
static inline u32 tsc_read32(void)
{
    u32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    (void)hi;
    return lo;
}

#endif
