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

static void pit_init(void)
{
    u16 divisor = (u16)PIT_DIVISOR;

    outb(PIT_CMD, 0x36);                   /* 通道0，低字节/高字节，模式3 方波 */
    outb(PIT_CH0, (u8)(divisor & 0xFFu));
    outb(PIT_CH0, (u8)(divisor >> 8));
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
