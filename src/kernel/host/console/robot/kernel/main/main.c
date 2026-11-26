#include "kernel/types.h"
#include "kernel/error.h"
#include "kernel/task.h"
#include "kernel/kthread.h"
#include "kernel/proc.h"
#include "kernel/fs/ramfs.h"
#include "kernel/mm/address_space.h"
#include "arch/x86/tss.h"
#include "arch/x86/syscall.h"
#include "arch/x86/page_fault.h"
#include "arch/x86/exception.h"
#include "arch/x86/interrupt.h"
#include "drivers/video/vga.h"
#include "drivers/serial/uart.h"
#include "app/arm/app.h"
#include "app/bench/bench.h"
#include "app/rtbench/rtbench.h"

void kernel_main(void)
{
#if !defined(AEVUM_SCHED_BENCH) && !defined(AEVUM_RT_BENCH)
    const char *rc;
#endif

    vga_init();
    uart_init();
    address_space_boot();
    task_boot();
    kthread_boot();
    process_boot();
    if (ramfs_init() != 0) {
        struct aevum_error e = {
            "ramfs",
            "image",
            "dependency",
            "P12.1 read-only ramdisk header/entry validation failed",
            "kernel_main -> ramfs_init"
        };
        aevum_fail(&e);
    }
    tss_init();

#if !defined(AEVUM_SCHED_BENCH) && !defined(AEVUM_RT_BENCH)
    rc = arm_app_init();
    if (rc) {
        struct aevum_error e = {
            "arm_app",
            rc,
            "dependency",
            "arm app task assembly failed",
            "kernel_main -> arm_app_init"
        };
        aevum_fail(&e);
    }
#endif

    /* 先装 IDT/PIC/PIT，再让第一个任务 iret 打开 IF；定时抢占从此刻开始。 */
    __asm__ __volatile__("cli");
    interrupt_init();
    syscall_init();
    exception_init();
    page_fault_init();
#ifdef AEVUM_SCHED_BENCH
    bench_start();
#elif defined(AEVUM_RT_BENCH)
    rtbench_start();
#else
    arm_app_start();
#endif
    for (;;) __asm__ __volatile__("cli; hlt");
}
