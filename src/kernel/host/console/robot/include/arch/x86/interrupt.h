#ifndef AEVUM_ARCH_X86_INTERRUPT_H
#define AEVUM_ARCH_X86_INTERRUPT_H

#include "kernel/types.h"

/*
 * 中断归属：IDT、8259A PIC 重映射、PIT 通道 0 和 LAPIC timer 由本模块
 * 统一初始化。IRQ0 门指向 loader.asm 的 irq0_stub，软件调度门指向
 * int40_stub，LAPIC 预算门指向 lapic_timer_stub。
 */
void interrupt_init(void);

/* 只给系统调用门使用：present, DPL=3, 32-bit interrupt gate。 */
void interrupt_set_user_gate(int vector, void (*handler)(void));

/* 只给异常门使用：present, DPL=0, 32-bit interrupt gate。 */
void interrupt_set_kernel_gate(int vector, void (*handler)(void));

/* IRQ0 处理完后由调度器调用，向主 PIC 发 EOI。 */
void interrupt_eoi(void);

/* RT-0b：返回当前 PIT 频率，用于 TSC 校准；不改变定时器。 */
u32 interrupt_tick_hz(void);

/*
 * P13.2：RT 预算定时器。LAPIC 可用时用 one-shot 每 1ms 伺服，最后
 * 一次事件标记 2ms 硬截断；LAPIC 不可用时回退到原来的 PIT one-shot。
 */
void interrupt_arm_rt_budget(u32 budget_us, u32 rest_us);
void interrupt_disarm_rt_budget(void);
/* 由 task_schedule_irq 在每次 IRQ0 入口调用；返回 1 表示本次是预算事件。 */
int interrupt_timer_event(void);
/* 由 task_schedule_irq 在 vector 0x30 入口调用；返回 1 表示本次是硬截断点。 */
int interrupt_lapic_event(void);
void interrupt_lapic_note_early(void);

int interrupt_rt_timer_is_lapic(void);
u32 interrupt_rt_servo_total(void);
u32 interrupt_rt_hard_total(void);
u32 interrupt_rt_ticks_per_ms(void);
u32 interrupt_rt_arm_total(void);
u32 interrupt_rt_gap_max_cycles(void);
u32 interrupt_rt_first_gap_cycles(void);
u32 interrupt_rt_cut_cycles(void);
u32 interrupt_rt_cut_tsc(void);
u32 interrupt_rt_cut_us(void);
u32 interrupt_rt_early_total(void);
u32 interrupt_rt_if_off_total(void);

#endif
