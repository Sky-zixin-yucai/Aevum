#ifndef AEVUM_ARCH_X86_LAPIC_H
#define AEVUM_ARCH_X86_LAPIC_H

#include "kernel/types.h"

/*
 * P13.2：本地 APIC 定时器 + HPET 参考。
 * HPET 只用于开机校准和 1ms 参考，不接管 100Hz PIT tick；LAPIC timer
 * 以 one-shot 方式在 RT 预算活动期间每 1ms 重装，最后一次事件做硬截断。
 */
int lapic_timer_init(void);
int lapic_timer_recalibrate(u32 cycles_per_us);
u32 lapic_timer_test_1ms_cycles(void);
u32 lapic_timer_cycles_per_us(void);
int lapic_timer_available(void);
u32 lapic_ticks_per_ms(void);

void lapic_timer_arm_us(u32 us);
void lapic_timer_stop(void);
void lapic_timer_eoi(void);

#endif
