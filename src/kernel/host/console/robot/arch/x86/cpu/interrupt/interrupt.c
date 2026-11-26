#include "arch/x86/interrupt.h"
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
    idt_set_gate_flags(0x40, int40_stub, 0x8E);

    idtr.limit = (u16)(sizeof(struct idt_entry) * IDT_ENTRIES - 1);
    idtr.base = IDT_BASE;
    __asm__ __volatile__("lidt %0" : : "m"(idtr));

    pic_remap();
    pit_init();
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
 * RT-2：预算子事件。arm 时把周期方波换成 2ms one-shot；预算 IRQ 处理完
 * 再装 rest_us 到下一个 10ms tick，tick IRQ 到达时恢复 100Hz 周期模式。
 * 只由 RT bench 的 ring0 控制任务调用，普通内核不进入该路径。
 */
void interrupt_arm_rt_budget(u32 budget_us, u32 rest_us)
{
    u32 flags;

    if (!budget_us || !rest_us) return;
    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    pit_rt_rest_us = rest_us;
    pit_mode = PIT_MODE_RT_BUDGET;
    pit_one_shot(budget_us);
    __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
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
