#include "arch/x86/interrupt.h"
#include "arch/x86/lapic.h"
#include "arch/x86/tsc.h"
#include "kernel/types.h"

#define IDT_BASE       0x12000u
#define IDT_ENTRIES    256
#define PIC1_CMD       0x20
#define PIC1_DATA      0x21
#define PIC2_CMD       0xA0
#define PIC2_DATA      0xA1
#define PIT_CMD        0x43
#define PIT_CH0        0x40
#define PIT_HZ         100u
#define PIT_DIVISOR    (1193182u / PIT_HZ)

#define PIT_MODE_PERIODIC 0
#define PIT_MODE_RT_BUDGET 1
#define PIT_MODE_RT_TICK   2

static volatile u32 pit_mode;
static u32 pit_rt_rest_us;

static volatile u32 lapic_active;
static u32 lapic_remaining_us;
static u32 lapic_armed_us;
static u32 lapic_servo_count;
static u32 lapic_hard_count;
static u32 lapic_arm_count;
static u32 lapic_last_tsc;
static u32 lapic_gap_max;
static u32 lapic_first_gap;
static u32 lapic_first_pending;
static u32 lapic_last_tsc_arm;
static u32 lapic_cut_cycles;
static u32 lapic_early_count;
static u32 lapic_arm_if_off;

struct idt_entry {
    u16 off_low;
    u16 selector;
    u8  zero;
    u8  flags;
    u16 off_high;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

extern void irq0_stub(void);
extern void lapic_timer_stub(void);
extern void int40_stub(void);

static struct idt_entry *const idt = (struct idt_entry *)IDT_BASE;
static struct idt_ptr idtr;

static inline void outb(u16 port, u8 value)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port)
{
    u8 value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void idt_set_gate_flags(int vector, void (*handler)(void), u8 flags)
{
    u32 address = (u32)handler;
    idt[vector].off_low = (u16)(address & 0xFFFFu);
    idt[vector].selector = 0x08;
    idt[vector].zero = 0;
    idt[vector].flags = flags;
    idt[vector].off_high = (u16)(address >> 16);
}

static void pic_remap(void)
{
    /* ICW1: 初始化；ICW2: 向量基址 0x20/0x28；ICW3: 主从连接；ICW4: 8086 模式。 */
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);
    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    /* 只放开 IRQ0，其它中断保持屏蔽。 */
    outb(PIC1_DATA, 0xFE);
    outb(PIC2_DATA, 0xFF);
    (void)inb(PIC1_DATA);
}

static void pit_periodic(void)
{
    u16 divisor = (u16)PIT_DIVISOR;

    pit_mode = PIT_MODE_PERIODIC;
    outb(PIT_CMD, 0x36);                   /* 通道0，低字节/高字节，模式3 方波 */
    outb(PIT_CH0, (u8)(divisor & 0xFFu));
    outb(PIT_CH0, (u8)(divisor >> 8));
}

/* RT-2：一次到期就停的 one-shot；下一次 IRQ 由事件处理重新装填。 */
static void pit_one_shot(u32 us)
{
    u32 divisor = (us * 1193u) / 1000u;

    if (!divisor) divisor = 1u;
    if (divisor > 65535u) divisor = 65535u;
    outb(PIT_CMD, 0x30);                   /* 通道0，模式0，中断触发后停 */
    outb(PIT_CH0, (u8)(divisor & 0xFFu));
    outb(PIT_CH0, (u8)(divisor >> 8));
}

static void pit_init(void)
{
    pit_periodic();
}

void interrupt_init(void)
{
    int i;

    for (i = 0; i < IDT_ENTRIES; i++) {
        idt[i].off_low = 0;
        idt[i].selector = 0;
        idt[i].zero = 0;
        idt[i].flags = 0;
        idt[i].off_high = 0;
    }

    idt_set_gate_flags(0x20, irq0_stub, 0x8E);
    idt_set_gate_flags(0x30, lapic_timer_stub, 0x8E);
    idt_set_gate_flags(0x40, int40_stub, 0x8E);

    idtr.limit = (u16)(sizeof(struct idt_entry) * IDT_ENTRIES - 1);
    idtr.base = IDT_BASE;
    __asm__ __volatile__("lidt %0" : : "m"(idtr));

    pic_remap();
    pit_init();
    /* P13.2：LAPIC + HPET 校准；失败时 RT 预算回退到 PIT one-shot。 */
    (void)lapic_timer_init();
}

void interrupt_set_user_gate(int vector, void (*handler)(void))
{
    idt_set_gate_flags(vector, handler, 0xEE);
}

void interrupt_set_kernel_gate(int vector, void (*handler)(void))
{
    idt_set_gate_flags(vector, handler, 0x8E);
}

void interrupt_eoi(void)
{
    outb(PIC1_CMD, 0x20);
}

u32 interrupt_tick_hz(void)
{
    return PIT_HZ;
}

/*
 * P13.2：LAPIC 可用时预算不再改 PIT 相位。每次 arm 至少装 1ms；
 * 1ms 到点时把剩余预算继续装成 one-shot，最后一次事件停止计数并
 * 由调度器做强制 throttle。
 */
void interrupt_arm_rt_budget(u32 budget_us, u32 rest_us)
{
    u32 flags;
    u32 first;

    if (!budget_us || !rest_us) return;
    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    if (!(flags & 0x200u)) lapic_arm_if_off++;

    if (lapic_timer_available()) {
        lapic_remaining_us = budget_us;
        lapic_active = 1;
        lapic_arm_count++;
        lapic_last_tsc = tsc_read32();
        lapic_last_tsc_arm = lapic_last_tsc;
        first = budget_us > 1000u ? 1000u : budget_us;
        lapic_armed_us = first;
        lapic_first_pending = 1;
        lapic_timer_arm_us(first);
        __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
        return;
    }

    /* 回退路径：原有 PIT one-shot 预算事件。 */
    pit_rt_rest_us = rest_us;
    pit_mode = PIT_MODE_RT_BUDGET;
    pit_one_shot(budget_us);
    __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
}

void interrupt_disarm_rt_budget(void)
{
    u32 flags;

    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    lapic_active = 0;
    lapic_remaining_us = 0;
    lapic_armed_us = 0;
    if (lapic_timer_available()) {
        lapic_timer_stop();
    } else if (pit_mode != PIT_MODE_PERIODIC) {
        pit_periodic();
    }
    __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
}

int interrupt_lapic_event(void)
{
    u32 armed;
    u32 now;
    u32 gap;

    if (!lapic_active) return 0;
    now = tsc_read32();
    if (lapic_last_tsc) {
        gap = now - lapic_last_tsc;
        if (gap > lapic_gap_max) lapic_gap_max = gap;
        if (lapic_first_pending) {
            lapic_first_gap = gap;
            lapic_first_pending = 0;
        }
    }
    lapic_last_tsc = now;
    lapic_servo_count++;
    armed = lapic_armed_us;
    if (armed >= lapic_remaining_us) {
        lapic_remaining_us = 0;
        lapic_active = 0;
        lapic_cut_cycles = now - lapic_last_tsc_arm;
        lapic_hard_count++;
        lapic_timer_stop();
        return 1;
    }
    lapic_remaining_us -= armed;
    armed = lapic_remaining_us > 1000u ? 1000u : lapic_remaining_us;
    lapic_armed_us = armed;
    lapic_timer_arm_us(armed);
    return 0;
}

void interrupt_lapic_note_early(void)
{
    lapic_early_count++;
}

u32 interrupt_rt_arm_total(void)
{
    return lapic_arm_count;
}

u32 interrupt_rt_gap_max_cycles(void)
{
    return lapic_gap_max;
}

u32 interrupt_rt_first_gap_cycles(void)
{
    return lapic_first_gap;
}

u32 interrupt_rt_cut_cycles(void)
{
    return lapic_cut_cycles;
}

u32 interrupt_rt_early_total(void)
{
    return lapic_early_count;
}

u32 interrupt_rt_if_off_total(void)
{
    return lapic_arm_if_off;
}

int interrupt_timer_event(void)
{
    if (pit_mode == PIT_MODE_RT_BUDGET) {
        /* 预算到期：先装到下一个 10ms tick，再让调度器记 CBS 账。 */
        pit_mode = PIT_MODE_RT_TICK;
        pit_one_shot(pit_rt_rest_us);
        return 1;
    }
    if (pit_mode == PIT_MODE_RT_TICK) {
        /* 10ms tick 到达：恢复周期方波，后续 IRQ 回到正常 100Hz。 */
        pit_periodic();
        return 0;
    }
    return 0;
}

int interrupt_rt_timer_is_lapic(void)
{
    return lapic_timer_available();
}

u32 interrupt_rt_servo_total(void)
{
    return lapic_servo_count;
}

u32 interrupt_rt_hard_total(void)
{
    return lapic_hard_count;
}

u32 interrupt_rt_ticks_per_ms(void)
{
    return lapic_ticks_per_ms();
}
