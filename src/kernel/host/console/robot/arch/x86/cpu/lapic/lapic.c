#include "arch/x86/lapic.h"
#include "arch/x86/tsc.h"
#include "config/memory.h"

#define IA32_APIC_BASE_MSR  0x1Bu
#define APIC_BASE_ENABLE    0x00000800u

#define LAPIC_SVR           0x0F0u
#define LAPIC_TDC           0x3E0u
#define LAPIC_TICR          0x380u
#define LAPIC_TCCR          0x390u
#define LAPIC_LVT_TIMER     0x320u
#define LAPIC_EOI           0x0B0u
#define LAPIC_TPR           0x080u
#define LAPIC_TIMER_VECTOR  0x30u

#define HPET_BASE           0xFED00000u
#define HPET_CAP            0x000u
#define HPET_CONFIG         0x010u
#define HPET_COUNTER        0x0F0u
#define HPET_CONFIG_ENABLE  0x001u

#define MMIO_PD             ((volatile u32 *)AEVUM_KERNEL_PD)
#define MMIO_PT             ((volatile u32 *)AEVUM_KERNEL_MMIO_PT)
#define MMIO_LIMIT          (AEVUM_KERNEL_MMIO_BASE + 0x00400000u)

static u32 lapic_base;
static u32 lapic_ready;
static u32 lapic_ticks_ms;
static u32 lapic_test_1ms_cycles;

static inline u32 mmio_read32(u32 addr)
{
    return *(volatile u32 *)addr;
}

static inline void mmio_write32(u32 addr, u32 value)
{
    *(volatile u32 *)addr = value;
}

static inline u32 lapic_read(u32 off)
{
    return mmio_read32(lapic_base + off);
}

static inline void lapic_write(u32 off, u32 value)
{
    mmio_write32(lapic_base + off, value);
}

static inline u32 read_cr3(void)
{
    u32 value;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(value));
    return value;
}

static inline void reload_cr3(void)
{
    u32 value = read_cr3();
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(value) : "memory");
}

static inline void read_msr(u32 msr, u32 *lo, u32 *hi)
{
    __asm__ __volatile__("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}

static inline void write_msr(u32 msr, u32 lo, u32 hi)
{
    __asm__ __volatile__("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

static int cpu_has_apic(void)
{
    u32 a, b, c, d;

    __asm__ __volatile__("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(1u));
    (void)a; (void)b; (void)c;
    return (int)((d >> 9) & 1u);
}

/*
 * 把 0xFEC00000..0xFF000000 的 4MB 窗口映射进当前页目录，页表放在
 * 0x15000；用户地址空间创建时也复制同一个 PDE，内核态访问 LAPIC/HPET
 * 不依赖当前 CR3 是内核目录还是用户目录。
 */
static int map_mmio(void)
{
    u32 i;
    u32 pd_index = AEVUM_KERNEL_MMIO_PDE;

    if (MMIO_PD[pd_index] == (AEVUM_KERNEL_MMIO_PT | 0x3u)) return 0;

    for (i = 0; i < 1024u; i++)
        MMIO_PT[i] = (AEVUM_KERNEL_MMIO_BASE + i * 0x1000u) | 0x3u;
    MMIO_PD[pd_index] = AEVUM_KERNEL_MMIO_PT | 0x3u;
    reload_cr3();
    return 0;
}

static u32 hpet_read32(u32 off)
{
    return mmio_read32(HPET_BASE + off);
}

static void hpet_write32(u32 off, u32 value)
{
    mmio_write32(HPET_BASE + off, value);
}

static inline u32 hpet_low_counter(void)
{
    return hpet_read32(HPET_COUNTER);
}

static inline void pit_outb(u16 port, u8 value)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 pit_inb(u16 port)
{
    u8 value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static u16 pit_read_count(void)
{
    u8 lo, hi;

    pit_outb(0x43, 0x00);      /* latch channel 0 */
    lo = pit_inb(0x40);
    hi = pit_inb(0x40);
    return (u16)lo | ((u16)hi << 8);
}

static void pit_calibration_start(void)
{
    /* channel 0, mode 0 one-shot，divisor 11932 ≈ 10ms。 */
    pit_outb(0x43, 0x30);
    pit_outb(0x40, 0x9C);
    pit_outb(0x40, 0x2E);
}

static void pit_periodic_restore(void)
{
    pit_outb(0x43, 0x36);
    pit_outb(0x40, 0x9C);
    pit_outb(0x40, 0x2E);
}

static int hpet_init(void)
{
    u32 cap_hi = hpet_read32(HPET_CAP + 4u);
    u32 config;

    if (!cap_hi) return -1;

    config = hpet_read32(HPET_CONFIG);
    if (!(config & HPET_CONFIG_ENABLE)) {
        hpet_write32(HPET_CONFIG, config | HPET_CONFIG_ENABLE);
        config = hpet_read32(HPET_CONFIG);
        if (!(config & HPET_CONFIG_ENABLE)) return -1;
    }
    return 0;
}

static int lapic_calibrate(void)
{
    u32 prev_pit, now_pit, delta, current, used, elapsed;
    u32 stall = 0;

    if (hpet_init()) return -1;

    /* 分频 1，LVT 先屏蔽，起一个很大的倒计数。 */
    lapic_write(LAPIC_TDC, 0x7u);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | (1u << 16));
    lapic_write(LAPIC_TICR, 0xFFFFFFFFu);

    /*
     * QEMU 的 HPET 计数频率和 capability 声明不一致，直接以 PIT 通道 0
     * 的 10ms one-shot 为参考测 LAPIC 倒计数速率；HPET 只负责启用/参考。
     */
    pit_calibration_start();
    prev_pit = pit_read_count();
    elapsed = 0;
    while (elapsed < 11931u) {
        now_pit = pit_read_count();
        delta = now_pit <= prev_pit ? prev_pit - now_pit : 0u;
        prev_pit = now_pit;
        if (!delta) {
            if (++stall > 1000000u) {
                pit_periodic_restore();
                return -1;
            }
        } else {
            stall = 0;
            elapsed += delta;
        }
    }
    pit_periodic_restore();

    current = lapic_read(LAPIC_TCCR);
    if (!current) return -1;
    used = 0xFFFFFFFFu - current;
    if (used < 100u) return -1;
    lapic_ticks_ms = used / 10u;
    if (!lapic_ticks_ms) return -1;

    lapic_write(LAPIC_TICR, 0u);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | (1u << 16));
    return 0;
}

int lapic_timer_init(void)
{
    u32 lo, hi, base, svr;

    if (lapic_ready) return 0;
    if (!cpu_has_apic()) return -1;

    read_msr(IA32_APIC_BASE_MSR, &lo, &hi);
    base = lo & 0xFFFFF000u;
    if (base < AEVUM_KERNEL_MMIO_BASE || base >= MMIO_LIMIT) return -1;
    if (!(lo & APIC_BASE_ENABLE)) {
        write_msr(IA32_APIC_BASE_MSR, lo | APIC_BASE_ENABLE, hi);
        read_msr(IA32_APIC_BASE_MSR, &lo, &hi);
        base = lo & 0xFFFFF000u;
    }

    if (map_mmio()) return -1;
    lapic_base = base;

    /* 打开 APIC，清任务优先级，定时器先屏蔽。 */
    svr = lapic_read(LAPIC_SVR);
    if (!(svr & 0x100u)) {
        lapic_write(LAPIC_SVR, 0x100u | 0x0FFu);
    }
    lapic_write(LAPIC_TPR, 0u);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | (1u << 16));
    lapic_timer_eoi();

    if (lapic_calibrate()) return -1;
    lapic_ready = 1;
    return 0;
}

int lapic_timer_available(void)
{
    return (int)lapic_ready;
}

u32 lapic_ticks_per_ms(void)
{
    return lapic_ticks_ms;
}

int lapic_timer_recalibrate(u32 cycles_per_us)
{
    u32 start, now, current, used;

    if (!lapic_ready || !cycles_per_us) return -1;

    lapic_write(LAPIC_TDC, 0x7u);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | (1u << 16));
    lapic_write(LAPIC_TICR, 0xFFFFFFFFu);

    start = tsc_read32();
    do {
        now = tsc_read32();
    } while ((u32)(now - start) < cycles_per_us * 10000u);

    current = lapic_read(LAPIC_TCCR);
    if (!current) return -1;
    used = 0xFFFFFFFFu - current;
    if (used < 100u) return -1;
    lapic_ticks_ms = used / 10u;

    /* 验证：屏蔽 LVT，只让 TCCR 倒计数到 0，分别用 TSC 和 TCCR 看 1ms。 */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | (1u << 16));
    lapic_write(LAPIC_TICR, lapic_ticks_ms);
    start = tsc_read32();
    do {
        now = tsc_read32();
    } while (lapic_read(LAPIC_TCCR) != 0u && (u32)(now - start) < 0x40000000u);
    lapic_test_1ms_cycles = now - start;

    lapic_write(LAPIC_TICR, 0u);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | (1u << 16));
    return lapic_ticks_ms ? 0 : -1;
}

u32 lapic_timer_test_1ms_cycles(void)
{
    return lapic_test_1ms_cycles;
}

void lapic_timer_arm_us(u32 us){
    u32 count;

    if (!lapic_ready || !us) return;
    count = (us * lapic_ticks_ms) / 1000u;
    if (!count) count = 1u;
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR);
    lapic_write(LAPIC_TICR, count);
}

void lapic_timer_stop(void)
{
    if (!lapic_ready) return;
    lapic_write(LAPIC_TICR, 0u);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | (1u << 16));
}

void lapic_timer_eoi(void)
{
    if (lapic_base) lapic_write(LAPIC_EOI, 0u);
}
