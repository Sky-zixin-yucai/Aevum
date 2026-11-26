#ifndef AEVUM_ARCH_X86_INTERRUPT_H
#define AEVUM_ARCH_X86_INTERRUPT_H

#include "kernel/types.h"

/*
 * 中断归属：IDT、8259A PIC 重映射、PIT 通道 0 只由本模块初始化。
 * IRQ0 门指向 loader.asm 的 irq0_stub，软件调度门指向 int40_stub。
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
 * RT-2：预算到期用 PIT 通道 0 的 one-shot 子事件，不替换 100Hz 周期 tick。
 * arm 后下一次 IRQ0 被标记为预算事件；事件处理完自动再装 rest_us 到下一个
 * 10ms tick，tick 到达时恢复周期模式。
 */
void interrupt_arm_rt_budget(u32 budget_us, u32 rest_us);
/* 由 task_schedule_irq 在每次 IRQ0 入口调用；返回 1 表示本次是预算事件。 */
int interrupt_timer_event(void);

#endif
