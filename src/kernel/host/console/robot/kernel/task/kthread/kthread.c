#include "kernel/kthread.h"
#include "kernel/task.h"
#include "kernel/task/stack.h"
#include "kernel/proc.h"

static u32 created_total;
static u32 exited_total;
static u32 reaped_total;
static u32 auto_reaped_total;
static u32 failed_total;
static u32 join_block_total;
static u32 join_wake_total;

/* 100Hz PIT：100 tick = 1s。给正常 join 留出调用窗口，晚到的 join 返回 -1。 */
#define KTHREAD_ZOMBIE_GRACE_TICKS 100u

struct kthread_join_ctx {
    int tid;
};

static int kthread_join_ready(void *opaque)
{
    struct kthread_join_ctx *ctx = (struct kthread_join_ctx *)opaque;
    struct task_info info;

    if (!task_snapshot_pid(ctx->tid, &info)) return 1;
    if (!info.kthread) return 1;
    return info.state == TASK_ZOMBIE;
}

void kthread_boot(void)
{
    created_total = 0;
    exited_total = 0;
    reaped_total = 0;
    auto_reaped_total = 0;
    failed_total = 0;
    join_block_total = 0;
    join_wake_total = 0;
}

int kthread_create(const char *name, void (*entry)(void *), void *arg,
                   int priority)
{
    struct task_struct *task;
    const char *rc;
    u32 stack_top;
    int tid;

    if (!name || !entry || priority < 0 || priority > 3) {
        failed_total++;
        return -1;
    }
    /* 动态内核线程只能由 ring0 任务创建，用户进程没有这条路径。 */
    if (task_process_current()) {
        failed_total++;
        return -1;
    }

    stack_top = kernel_stack_alloc();
    if (!stack_top) {
        failed_total++;
        return -1;
    }

    rc = task_create_kernel_arg(priority, name, entry, arg, stack_top, &task);
    if (rc) {
        kernel_stack_free(stack_top);
        failed_total++;
        return -1;
    }

    tid = task_pid_of(task);
    created_total++;
    return tid;
}

void kthread_exit(u32 code)
{
    /*
     * 只有带 kthread 标记的任务才能走这里；静态 control/render/idle
     * 若误调用，直接停住而不是把常驻任务变成无人回收的 ZOMBIE。
     */
    if (!task_current_is_kthread())
        for (;;) __asm__ __volatile__("cli; hlt");

    task_set_current_exit_code(code);
    exited_total++;
    task_exit();
}

int kthread_join(int tid, u32 *status)
{
    struct kthread_join_ctx ctx;
    struct task_info info;
    int rc;

    if (tid <= 0 || tid == task_current_pid()) return -1;

    ctx.tid = tid;
    task_set_kthread_wait(tid);

    for (;;) {
        if (!task_snapshot_pid(tid, &info)) {
            task_set_kthread_wait(0);
            return -1;
        }
        if (!info.kthread) {
            task_set_kthread_wait(0);
            return -1;
        }
        if (info.state == TASK_ZOMBIE) break;

        rc = task_block_until(kthread_join_ready, &ctx);
        if (rc < 0) {
            task_set_kthread_wait(0);
            return -1;
        }
        if (rc > 0) {
            join_block_total++;
            join_wake_total++;
        }
    }

    if (status) *status = info.exit_code;
    if (task_reap_kthread(tid) != 0) {
        task_set_kthread_wait(0);
        return -1;
    }
    task_set_kthread_wait(0);
    reaped_total++;
    return 0;
}

int kthread_self(void)
{
    return task_current_is_kthread() ? task_current_pid() : -1;
}

void kthread_reap_unjoined(void)
{
    int n = task_reap_unjoined_kthread(task_tick_total(),
                                       KTHREAD_ZOMBIE_GRACE_TICKS);
    if (n > 0) {
        reaped_total += (u32)n;
        auto_reaped_total += (u32)n;
    }
}

void kthread_stats(struct kthread_stats *out)
{
    if (!out) return;
    out->created = created_total;
    out->exited = exited_total;
    out->reaped = reaped_total;
    out->auto_reaped = auto_reaped_total;
    out->failed = failed_total;
    out->join_blocks = join_block_total;
    out->join_wakes = join_wake_total;
    out->live = created_total - reaped_total;
}
