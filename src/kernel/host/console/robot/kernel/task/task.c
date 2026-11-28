#include "kernel/task.h"
#include "kernel/task/family.h"
#include "kernel/task/stack.h"
#include "kernel/kthread.h"
#include "kernel/proc.h"
#include "arch/x86/interrupt.h"
#include "arch/x86/lapic.h"
#include "arch/x86/frame.h"
#include "arch/x86/tss.h"
#include "arch/x86/tsc.h"
#include "kernel/mm/address_space.h"
#include "config/memory.h"

#define CTX_FRAME_SLOTS      12
#define CTX_FRAME_SLOTS_USER 14
#define TASK_TRACE_N    64
#define TASK_TRACE_DIV  32
#define PID_MAX         31
#define STACK_MAGIC     0xA5A5A5A5u

struct context {
    u32 esp;
};

static void task_fill_info(struct task_struct *task, struct task_info *out);
static void task_kernel_arg_entry(void);

struct task_struct {
    int  pid;       /* TID：调度实体 ID */
    int  tgid;      /* 线程组 ID：同进程线程共享 */
    int  state;
    int  priority;
    int  base_priority;  /* P13.5：用户可见的基准优先级 */
    int  pi_priority;    /* P13.5：继承来的最高等待者优先级；无继承时等于 base */
    u8   pi_boosted;     /* P13.5：1 表示当前 priority 来自 PI 提升 */
    char comm[TASK_NAME_MAX + 1];
    u32  stack_base;
    u32  stack_top;
    u32  switches;
    u32  cpu_ticks;
    u32  exit_code;
    int  detached;  /* 1：线程退出后由调度器自动回收，不等待 join */
    int  kthread;   /* 1：P8 动态内核线程，ZOMBIE 由 join 或调度器兜底回收 */
    int  rt;        /* RT-1：最小 RT 类；只给控制观测任务，优先于 fair 类 */
    u32  rt_budget_cycles;  /* RT-2：每周期 CBS 预算，TSC 周期；0 表示不限制 */
    u32  rt_budget_left;    /* 本周期剩余预算 */
    u32  rt_budget_last_tsc;/* 上次开始/记账的 TSC；0 表示尚未开始 */
    u32  rt_after_cut_cycles;/* 硬截断后、本次作业返回前实际执行的 TSC 周期 */
    u32  rt_throttle_until; /* throttle 到该 tick 后解除 */
    u16  rt_period_ticks;   /* 预算补满周期，100Hz 下 10ms=1 tick */
    u8   rt_throttled;      /* 1：预算耗尽，调度器在本周期内跳过 */
    u8   rt_preempted;      /* 1：本次作业已被 LAPIC 硬截断 */
    u8   rt_prev;           /* P13.1：RT 双向队列前驱槽；0xFF 表示无 */
    u8   rt_next;           /* P13.1：RT 双向队列后继槽；0xFF 表示无 */
    u8   rt_prio;           /* P13.1：入队时的 RT 优先级，出队用 */
    u8   rt_queued;         /* P13.1：1 表示已在 RT 就绪队列中 */
    int  kthread_wait_tid; /* >0：当前任务正阻塞等待该 TID 的 kthread */
    u32  wake_tick;         /* P11.1：定时阻塞的到期 PIT tick；0 表示无限等待 */
    struct wait_queue *waitq; /* P11.3：当前阻塞所属对象等待队列；0 表示普通等待 */
    u32  wait_ticket;       /* P11.6：队列内 FIFO 票据；0 表示未排队 */
    u32  slice_left;        /* P11.5：当前时间片剩余 PIT tick */
    u32  zombie_ticks;     /* 进入 ZOMBIE 时的 PIT tick，供未 join kthread 计龄 */
    u32  kernel_entry;     /* kthread 入口函数指针 */
    u32  kernel_arg;       /* kthread 入口参数 */
    u64  vruntime;
    u32  fpu_state;
    int  user;
    struct address_space *space;
    struct process *proc;
    struct context ctx;
};

enum schedule_reason {
    SCHED_START = 0,
    SCHED_YIELD = 1,
    SCHED_BLOCK = 2,
    SCHED_EXIT = 3,
    SCHED_TICK = 4
};

static struct task_struct tasks[TASK_MAX];
static struct task_struct boot_task;
static struct task_struct *current_task;
static struct task_struct *start_target;
static u32 tick_total;

/* P13.1：RT 类独立就绪队列。优先级直接复用 priority 0..3，0 最高。 */
#define TASK_RT_PRIO_COUNT (TASK_PRIORITY_MAX + 1)
#define TASK_RT_NONE 0xFFu
static u8 rt_head[TASK_RT_PRIO_COUNT];
static u8 rt_tail[TASK_RT_PRIO_COUNT];
static u8 rt_mask;

static volatile u32 sched_reason;
static volatile int sched_failed;

static u32 switch_trace[TASK_TRACE_N];
static int switch_i;
static u32 switch_total;
static u32 trace_total;
static u32 pid_map;
static u32 preempt_total;
/* P11.1：只统计显式 sleep 和所有定时阻塞到期唤醒。 */
static u32 sleep_block_total;
static u32 sleep_wake_total;
static u32 timeout_wake_total;

/* RT-0b：只记录一个探针任务的定时唤醒时刻；不改变调度路径。 */
static struct task_struct *rt_probe_task;
static u32 rt_probe_wake_tsc;
/* P11.3：对象等待队列的阻塞/唤醒/超时统计，只由 task_block_on 记账。 */
static u32 io_block_total;
static u32 io_wake_total;
static u32 io_timeout_total;
/* P11.6：FIFO 定向唤醒与最大等待深度。 */
static u32 wq_wake_one_total;
static u32 wq_wake_all_total;
static u32 wq_max_depth;
/* P11.4：idle 只在没有其它 READY 任务时被选中；hlt 次数单独记账。 */
static struct task_struct *idle_task;
static u32 idle_hlt_total;
/* B 键演示：绕过“有非 idle READY 就不选 idle”的规则，强制选中 idle 一次。 */
static volatile u32 idle_demo_pick;
/* P11.5：显式时间片的到期与“片未尽、跳过重选”计数。 */
static u32 slice_expire_total;
static u32 tick_defer_total;
/* RT-1：唤醒后需要抢占当前任务的标记；只在 IRQ0 出口消费。 */
static volatile u32 need_resched;
static u32 wake_resched_total;
/* RT-2：CBS 超预算与 throttle 次数；按任务字段记账，这里只汇总。 */
static u32 rt_budget_overrun_total;
static u32 rt_budget_throttle_total;
static u32 rt_preempt_total;
/* P13.1：RT bench 的选任务周期统计；普通构建仍为 0，不增加 IRQ0 开销。 */
static u32 pick_cycles_max;
static u32 pick_cycles_total;
static u32 pick_calls;
/* P11.7：被 pending signal 打断的阻塞次数。 */
static u32 eintr_total;
/*
 * P11.9：CFS-lite 的全局虚拟时间下界。只增不减；睡眠任务被唤醒时
 * 钳到该值，避免长时间睡眠后带着过期的小 vruntime 一次性抢走 CPU。
 */
static u64 min_vruntime;
#define WEIGHT_QUANTUM 64u

/* FXSAVE 区域必须 16 字节对齐；模板只用于新任务的初始 FPU/SSE 状态。 */
static u8 fpu_template[AEVUM_FPU_STATE_SIZE] __attribute__((aligned(16)));
static u8 boot_fpu_state[AEVUM_FPU_STATE_SIZE] __attribute__((aligned(16)));

static inline void fpu_save(void *area)
{
    __asm__ __volatile__("fxsave (%0)" : : "r"(area) : "memory");
}

static inline void fpu_restore(void *area)
{
    __asm__ __volatile__("fxrstor (%0)" : : "r"(area) : "memory");
}

/*
 * P11.9：priority 0 最高、3 最低，依次对应权重 8/4/2/1。
 * WEIGHT_QUANTUM=64 能被这四个权重整除，逐 tick 记账没有余数损失；
 * 因此 4 个 CPU-bound 任务在整 tick 账本上应得到 8:4:2:1 的份额。
 */
static u32 task_weight(const struct task_struct *task)
{
    return 1u << (TASK_PRIORITY_MAX - task->priority);
}

/*
 * 只把非 idle 的 READY/RUNNING 任务纳入 min_vruntime；idle 永远有
 * vruntime 0，若把它算进来，下界会永久卡在 0，睡眠任务仍能带着旧信用
 * 唤醒。
 */
static void task_vruntime_refresh_min(void)
{
    u64 m = 0;
    int found = 0;
    int i;

    for (i = 0; i < TASK_MAX; i++) {
        struct task_struct *t = &tasks[i];
        if (t == idle_task) continue;
        if (t->state != TASK_READY && t->state != TASK_RUNNING) continue;
        if (!found || t->vruntime < m) {
            m = t->vruntime;
            found = 1;
        }
    }
    if (found && m > min_vruntime) min_vruntime = m;
}

/* 唤醒前先刷新下界，再把过期的小 vruntime 钳到 min_vruntime。 */
static void task_vruntime_wake(struct task_struct *t)
{
    task_vruntime_refresh_min();
    if (t->vruntime < min_vruntime) t->vruntime = min_vruntime;
}

static int pid_alloc(void)
{
    int i;
    for (i = 0; i < PID_MAX; i++) {
        u32 mask = 1u << i;
        if (!(pid_map & mask)) {
            pid_map |= mask;
            return i + 1;
        }
    }
    return 0;
}

static void pid_free(int pid)
{
    if (pid >= 1 && pid <= PID_MAX) pid_map &= ~(1u << (pid - 1));
}

/*
 * P13.1：RT 就绪队列 helper。
 * 队列只收 rt && READY && !rt_throttled 的任务；RUNNING/BLOCKED/ZOMBIE
 * 都不在队列里。双向链用槽号 0..TASK_MAX-1，TASK_RT_NONE 表示空。
 */
static int task_slot_index(const struct task_struct *task)
{
    u32 base = (u32)(unsigned long)tasks;
    u32 addr = (u32)(unsigned long)task;
    u32 span = (u32)sizeof(tasks);

    if (addr < base || addr >= base + span) return -1;
    return (int)((addr - base) / (u32)sizeof(tasks[0]));
}

static void rt_dequeue(struct task_struct *task)
{
    int idx;
    u8 prev, next, prio;

    if (!task || !task->rt_queued) return;
    idx = task_slot_index(task);
    if (idx < 0) {
        task->rt_queued = 0;
        task->rt_prev = TASK_RT_NONE;
        task->rt_next = TASK_RT_NONE;
        return;
    }
    prev = task->rt_prev;
    next = task->rt_next;
    prio = task->rt_prio;
    if (prio >= TASK_RT_PRIO_COUNT) prio = 0;

    if (prev != TASK_RT_NONE) {
        tasks[prev].rt_next = next;
    } else {
        rt_head[prio] = next;
    }
    if (next != TASK_RT_NONE) {
        tasks[next].rt_prev = prev;
    } else {
        rt_tail[prio] = prev;
    }
    if (rt_head[prio] == TASK_RT_NONE) rt_mask &= (u8)~(1u << prio);

    task->rt_prev = TASK_RT_NONE;
    task->rt_next = TASK_RT_NONE;
    task->rt_queued = 0;
}

static void rt_enqueue(struct task_struct *task)
{
    int idx;
    u8 prio;

    if (!task || !task->rt || task->rt_queued) return;
    if (task->state != TASK_READY || task->rt_throttled) return;
    idx = task_slot_index(task);
    if (idx < 0) return;
    prio = (task->priority >= 0 && task->priority <= TASK_PRIORITY_MAX)
               ? (u8)task->priority : 0;

    task->rt_prio = prio;
    task->rt_prev = rt_tail[prio];
    task->rt_next = TASK_RT_NONE;
    if (rt_tail[prio] != TASK_RT_NONE) {
        tasks[rt_tail[prio]].rt_next = (u8)idx;
    } else {
        rt_head[prio] = (u8)idx;
    }
    rt_tail[prio] = (u8)idx;
    rt_mask |= (u8)(1u << prio);
    task->rt_queued = 1;
}

static struct task_struct *rt_pop_head(int prio)
{
    struct task_struct *task;
    u8 idx;

    if (prio < 0 || prio >= TASK_RT_PRIO_COUNT) return 0;
    idx = rt_head[prio];
    if (idx == TASK_RT_NONE) return 0;
    task = &tasks[idx];
    rt_dequeue(task);
    return task;
}

static int rt_lowest_priority(void)
{
    int prio;

    for (prio = 0; prio < TASK_RT_PRIO_COUNT; prio++) {
        if (rt_mask & (u8)(1u << prio)) return prio;
    }
    return -1;
}

/* 所有 READY 写入口都走这里：先改状态，再按 RT 规则决定是否入队。 */
static void task_make_ready(struct task_struct *task)
{
    if (!task) return;
    task->state = TASK_READY;
    rt_enqueue(task);
}

static void task_make_running(struct task_struct *task)
{
    if (!task) return;
    rt_dequeue(task);
    task->state = TASK_RUNNING;
}

static void rt_queue_reset(void)
{
    int i;

    for (i = 0; i < TASK_RT_PRIO_COUNT; i++) {
        rt_head[i] = TASK_RT_NONE;
        rt_tail[i] = TASK_RT_NONE;
    }
    rt_mask = 0;
}

static void task_zero(struct task_struct *task)
{
    int i;
    rt_dequeue(task);
    task->pid = 0;
    task->tgid = 0;
    task->state = TASK_UNUSED;
    task->priority = 0;
    for (i = 0; i <= TASK_NAME_MAX; i++) task->comm[i] = 0;
    task->stack_base = 0;
    task->stack_top = 0;
    task->switches = 0;
    task->cpu_ticks = 0;
    task->exit_code = 0;
    task->detached = 0;
    task->kthread = 0;
    task->rt = 0;
    task->rt_budget_cycles = 0;
    task->rt_budget_left = 0;
    task->rt_budget_last_tsc = 0;
    task->rt_after_cut_cycles = 0;
    task->rt_period_ticks = 0;
    task->rt_throttle_until = 0;
    task->rt_throttled = 0;
    task->rt_preempted = 0;
    task->rt_prev = TASK_RT_NONE;
    task->rt_next = TASK_RT_NONE;
    task->rt_prio = 0;
    task->rt_queued = 0;
    task->kthread_wait_tid = 0;
    task->wake_tick = 0;
    task->waitq = 0;
    task->wait_ticket = 0;
    task->slice_left = 0;
    task->zombie_ticks = 0;
    task->kernel_entry = 0;
    task->kernel_arg = 0;
    task->vruntime = 0;
    task->fpu_state = 0;
    task->user = 0;
    task->space = 0;
    task->proc = 0;
    task->ctx.esp = 0;
}

void task_boot(void)
{
    task_zero(&boot_task);
    rt_queue_reset();
    boot_task.state = TASK_RUNNING;
    boot_task.slice_left = TASK_SLICE_TICKS;
    boot_task.comm[0] = 'i';
    boot_task.comm[1] = 'n';
    boot_task.comm[2] = 'i';
    boot_task.comm[3] = 't';
    boot_task.fpu_state = (u32)boot_fpu_state;
    boot_task.space = address_space_kernel();
    current_task = &boot_task;
    pid_map = 0;
    family_boot();

    /*
     * 先给当前 FPU 一个已知初态，再分别保存模板和 boot 任务。
     * 模板不会被切换路径改写，新任务从它复制，避免 fxrstor 到未初始化区域。
     */
    __asm__ __volatile__("fninit");
    fpu_save(fpu_template);
    fpu_save(boot_fpu_state);
}

static const char *task_create_common(int priority, const char *name, u32 entry,
                              u32 stack_top, struct address_space *space,
                              int user, u32 user_stack, struct process *proc,
                              int pid_hint, void (*kernel_entry)(void *),
                              void *kernel_arg, int kthread,
                              struct task_struct **out)
{
    struct task_struct *task;
    u32 *sp;
    u32 *p;
    u32 stack_base;
    u32 fpu_state;
    int i, pid;

    if (!out) return "out";
    *out = 0;
    if (!entry) return "entry";
    if (!space) return "space";
    if (priority < 0 || priority > TASK_PRIORITY_MAX) return "priority";
    if (stack_top < AEVUM_TASK_STACK_MIN || stack_top > AEVUM_TASK_STACK_MAX ||
        (stack_top & 0xFu))
        return "stack";
    if (user && !proc) return "user/proc";
    if (!user && proc) return "user/proc";
    if (kthread && (!kernel_entry || user || proc)) return "kthread";

    stack_base = stack_top - AEVUM_TASK_STACK_SIZE;
    if (stack_base < AEVUM_TASK_STACK_MIN) return "stack";

    /*
     * 每个任务栈底部预留 512 字节 FXSAVE 区，余下 3.5KB 才是可用栈。
     * 这样不把 6×512 字节塞进 BSS，也不越过 BSS 与任务栈之间的保护空隙。
     */
    if (stack_base + AEVUM_FPU_STATE_SIZE > stack_top)
        return "stack";
    fpu_state = stack_base;
    stack_base += AEVUM_FPU_STATE_SIZE;
    if (stack_base > stack_top) return "stack";

    task = 0;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            task = &tasks[i];
            break;
        }
    }
    if (!task) return "slot";

    if (pid_hint) {
        if (pid_hint < 1 || pid_hint > PID_MAX) return "pid_hint.range";
        if (!(pid_map & (1u << (pid_hint - 1)))) return "pid_hint.used";
        pid = pid_hint;
    } else {
        pid = pid_alloc();
        if (!pid) return "pid_pool";
    }

    task_zero(task);
    task->pid = pid;
    task->tgid = proc ? process_pid_of(proc) : pid;
    task->priority = priority;
    task->base_priority = priority;
    task->pi_priority = priority;
    /* P11.5：新任务从完整时间片开始，避免落在 0 片立即被抢占。 */
    task->slice_left = TASK_SLICE_TICKS;
    /* P11.9：新任务从全局虚拟时间下界起跑，不继承当前任务已经积累的
     * 高 vruntime，也不拿 0 值去抢占已运行任务的时间片。先刷新一次，
     * 把当前 RUNNING 任务也算进下界，避免片未尽的窗口里新任务拿到旧值。 */
    task_vruntime_refresh_min();
    task->vruntime = min_vruntime;
    task->stack_base = stack_base;
    task->stack_top = stack_top;
    task->fpu_state = fpu_state;
    task->user = user;
    task->space = space;
    task->proc = proc;
    task->exit_code = 0;
    task->kthread = kthread;
    task->kernel_entry = (u32)kernel_entry;
    task->kernel_arg = (u32)kernel_arg;
    for (i = 0; i < TASK_NAME_MAX && name[i]; i++) task->comm[i] = name[i];
    task->comm[i] = 0;

    /* 用户进程的父子关系归 process 表；family 只维护内核任务树。 */
    if (!proc)
        family_add(pid, current_task ? current_task->pid : 0);

    /* 新任务的 FPU/SSE 从 boot 初始模板复制，不要从零初始化。 */
    for (i = 0; i < (int)(AEVUM_FPU_STATE_SIZE / 4); i++)
        ((u32 *)fpu_state)[i] = ((u32 *)fpu_template)[i];

    /* 栈底到栈顶填魔数，运行后从栈底向上扫描即可得到真实栈水位。 */
    for (p = (u32 *)stack_base; p < (u32 *)stack_top; p++) *p = STACK_MAGIC;

    /*
     * 新任务的初始帧必须和中断帧同构，第一次 iret 就会进入 entry。
     * ring0 用 0x08 代码段；ring3 用 0x1B/0x23，并在帧尾补 user esp/ss。
     * eflags=0x202 打开 IF，任务开始运行后 PIT 才能真正抢占。
     */
    sp = (u32 *)(stack_top - (user ? CTX_FRAME_SLOTS_USER : CTX_FRAME_SLOTS) * 4);
    for (i = 0; i < (user ? CTX_FRAME_SLOTS_USER : CTX_FRAME_SLOTS); i++) sp[i] = 0;
    sp[9]  = entry;
    sp[10] = user ? 0x1B : 0x08;
    sp[11] = 0x202;
    if (user) {
        sp[12] = user_stack;
        sp[13] = 0x23;
    }
    task->ctx.esp = (u32)sp;

    /*
     * 新任务必须在完全构造好之后才发布为 READY。
     * 否则 PIT 可能在 task_create 中途抢占，调度器会选中 ctx.esp 还无效的任务。
     */
    {
        u32 flags;
        __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
        task_make_ready(task);
        *out = task;
        __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
    }
    return 0;
}

const char *task_create(int priority, const char *name,
                void (*entry)(void), u32 stack_top, struct task_struct **out)
{
    return task_create_common(priority, name, (u32)entry, stack_top,
                              address_space_kernel(), 0, 0, 0, 0, 0, 0, 0, out);
}

const char *task_create_kernel_arg(int priority, const char *name,
                                   void (*entry)(void *), void *arg,
                                   u32 stack_top, struct task_struct **out)
{
    if (!entry) return "entry";
    return task_create_common(priority, name, (u32)task_kernel_arg_entry,
                              stack_top, address_space_kernel(), 0, 0, 0, 0,
                              entry, arg, 1, out);
}

/*
 * 内核任务初始帧没有参数寄存器语义，入口统一走这个 trampoline：
 * task_struct 里保存 fn/arg，真正进入 ring0 后才解引用并调用。
 */
static void task_kernel_arg_entry(void)
{
    void (*fn)(void *) = (void (*)(void *))current_task->kernel_entry;
    void *arg = (void *)current_task->kernel_arg;

    fn(arg);
    task_set_current_exit_code(0);
    task_exit();
}

const char *task_create_user_with_pid(int priority, const char *name, u32 user_eip,
                              u32 kernel_stack_top, struct address_space *space,
                              u32 user_stack_top, struct process *proc, int pid,
                              struct task_struct **out)
{
    if (!space || !proc || pid <= 0) return "space/proc/pid";
    return task_create_common(priority, name, user_eip, kernel_stack_top,
                              space, 1, user_stack_top, proc, pid, 0, 0, 0, out);
}

const char *task_create_thread_user(int tid, const char *name,
                                    struct process *proc,
                                    struct address_space *space, u32 user_eip,
                                    u32 kernel_stack_top, u32 user_stack_top,
                                    u32 arg, struct task_struct **out)
{
    struct task_struct *task;
    u32 *sp;
    const char *rc;

    if (!space || !proc || tid <= 0) return "space/proc/tid";
    /* P11.2：clone 线程继承当前线程优先级，不再硬编码 1。 */
    rc = task_create_common(current_task ? current_task->priority : 1,
                            name, user_eip, kernel_stack_top, space,
                            1, user_stack_top, proc, tid, 0, 0, 0, out);
    if (rc) return rc;

    task = *out;
    /* 线程入口从 eax 读 arg；首轮必须有机会跑，父线程随后 yield/join。 */
    sp = (u32 *)task->ctx.esp;
    sp[0] = arg;
    /* P11.9：线程继承全局下界；父线程随后 yield/join，子线程不会因 0 值
     * 获得超过一整轮的历史信用。 */
    task->vruntime = min_vruntime;
    return 0;
}

int task_pid_reserve(void)
{
    return pid_alloc();
}

/* fork 可指定刚回收的子 PID，避免无关墓碑被新进程覆盖。 */
int task_pid_reserve_hint(int pid)
{
    u32 mask;
    if (pid <= 0 || pid > PID_MAX) return 0;
    mask = 1u << (pid - 1);
    if (pid_map & mask) return 0;
    pid_map |= mask;
    return pid;
}

void task_pid_release(int pid)
{
    pid_free(pid);
}

/*
 * fork 的子任务不是从模板起跑：复制父进程的 ring3 中断帧、通用寄存器和
 * FPU 状态，只把 eax 改成 0。用户栈页已由 address_space_clone() 复制。
 */
const char *task_fork_user(int pid, const char *name, struct irq_frame *frame,
                   u32 kernel_stack_top, struct address_space *space,
                   struct process *proc, struct task_struct **out)
{
    struct task_struct *task;
    u32 *sp;
    const char *rc;
    int i;

    if (!frame || !space || !proc || pid <= 0) return "frame/space/proc/pid";
    /* P11.2：fork 子进程继承父进程优先级。 */
    rc = task_create_common(current_task ? current_task->priority : 1,
                            name, frame->eip, kernel_stack_top, space,
                            1, frame->user_esp, proc, pid, 0, 0, 0, out);
    if (rc) return rc;
    task = *out;
    /* fork 子任务必须有机会先跑一次：父进程随后进入 wait。
     * P11.9 用全局下界而不是 0：子任务仍在父进程之前被选中，但不会拿到
     * 超过 min_vruntime 的历史信用，唤醒后的份额更接近 CFS。 */
    task->vruntime = min_vruntime;
    sp = (u32 *)task->ctx.esp;
    sp[0] = 0;                 /* 子进程 fork 返回 0 */
    sp[1] = frame->ebx;
    sp[2] = frame->ecx;
    sp[3] = frame->edx;
    sp[4] = frame->esi;
    sp[5] = frame->edi;
    sp[6] = frame->ebp;
    sp[11] = frame->eflags;
    sp[12] = frame->user_esp;
    sp[13] = frame->ss;

    /* 子进程继承父进程的 XMM/FPU 现场，而不是 boot 模板。 */
    fpu_save((void *)current_task->fpu_state);
    for (i = 0; i < (int)(AEVUM_FPU_STATE_SIZE / 4); i++)
        ((u32 *)task->fpu_state)[i] = ((u32 *)current_task->fpu_state)[i];
    return 0;
}

struct task_struct *task_current(void)
{
    return current_task;
}

int task_current_pid(void)
{
    return current_task ? current_task->pid : 0;
}

int task_pid_of(struct task_struct *task)
{
    return task ? task->pid : 0;
}

struct process *task_process_current(void)
{
    return current_task ? current_task->proc : 0;
}

struct process *task_process_of(struct task_struct *task)
{
    return task ? task->proc : 0;
}

static struct task_struct *task_find_pid(int tid)
{
    int i;

    if (tid == 0) return current_task;
    if (tid < 0) return 0;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == tid)
            return &tasks[i];
    }
    return 0;
}

int task_priority_get(int tid)
{
    struct task_struct *task = task_find_pid(tid);

    if (!task || task->state == TASK_UNUSED) return -1;
    /* 用户态不读取内核静态任务的调度参数。 */
    if (current_task && current_task->user && !task->user) return -1;
    return task->priority;
}

int task_priority_set(int tid, int priority)
{
    struct task_struct *task;
    u32 flags;
    int rc = -1;
    int caller_user;
    int old_priority;

    if (priority < 0 || priority > TASK_PRIORITY_MAX) return -1;
    caller_user = current_task && current_task->user;
    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    task = task_find_pid(tid);
    if (task && task->state != TASK_UNUSED && task->state != TASK_ZOMBIE &&
        (!caller_user || task->user)) {
        /*
         * 只改后续 PIT 记账权重，不重算历史 vruntime。这样反复
         * setpriority 不能把累计虚拟时间清零来刷调度优势。
         * P13.1：RT 任务若已在就绪队列，先摘链再改优先级，避免槽号
         * 还挂在旧优先级队列上。
         * P13.5：priority 是有效优先级，base_priority 才是用户设定值；
         * 若当前正被 PI 提升，改基准后仍取 base 与继承值的较小者。
         */
        rt_dequeue(task);
        old_priority = task->priority;
        task->base_priority = priority;
        task->priority = (task->pi_boosted && task->pi_priority < priority)
                             ? task->pi_priority : priority;
        if (task->state == TASK_READY) rt_enqueue(task);
        if (task == current_task && task->state == TASK_RUNNING &&
            task->priority > old_priority)
            need_resched = 1;
        rc = 0;
    }
    __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
    return rc;
}

/*
 * P13.5/P13.6：RT 互斥锁的优先级继承/天花板后端。只有 RT 类 owner 需要
 * 提升；fair owner 的 priority 不参与 RT 队列，提升它没有意义。
 * 返回值 1 表示 owner 属于 RT 类并接受了提升，0 表示不需要/不可提升。
 */
int task_rt_mutex_boost(struct task_struct *task, int priority)
{
    u32 flags;
    int old_priority;
    int target;

    if (!task || !task->rt || priority < 0 || priority > TASK_PRIORITY_MAX)
        return 0;

    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    if (!task->pi_boosted) {
        task->pi_priority = priority;
        task->pi_boosted = 1;
    } else if (priority < task->pi_priority) {
        task->pi_priority = priority;
    }

    target = (task->pi_priority < task->base_priority)
                 ? task->pi_priority : task->base_priority;
    old_priority = task->priority;
    if (target != old_priority) {
        rt_dequeue(task);
        task->priority = target;
        if (task->state == TASK_READY) rt_enqueue(task);
        if (task == current_task && task->state == TASK_RUNNING &&
            target < old_priority)
            need_resched = 1;
    }
    __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
    return 1;
}

/* 解锁后把 owner 的有效优先级恢复到基准值；嵌套锁不在 P13.5 范围内。 */
int task_rt_mutex_restore(struct task_struct *task)
{
    u32 flags;
    int old_priority;

    if (!task || !task->pi_boosted) return 0;

    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    old_priority = task->priority;
    rt_dequeue(task);
    task->pi_boosted = 0;
    task->pi_priority = task->base_priority;
    task->priority = task->base_priority;
    if (task->state == TASK_READY) rt_enqueue(task);
    if (task == current_task && task->state == TASK_RUNNING &&
        task->priority > old_priority)
        need_resched = 1;
    __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
    return 1;
}


void task_set_space(struct task_struct *task, struct address_space *space)
{
    if (task && space) task->space = space;
}

char task_state_char(int state)
{
    switch (state) {
        case TASK_RUNNING: return 'R';
        case TASK_READY:   return 'r';
        case TASK_BLOCKED: return 'B';
        case TASK_ZOMBIE:  return 'Z';
        default:           return '?';
    }
}

const char *task_state_name(int state)
{
    switch (state) {
        case TASK_RUNNING: return "RUN";
        case TASK_READY:   return "READY";
        case TASK_BLOCKED: return "BLOCK";
        case TASK_ZOMBIE:  return "ZOMB";
        default:           return "?";
    }
}

int task_pid_used(void)
{
    int i, n = 0;
    for (i = 0; i < PID_MAX; i++)
        if (pid_map & (1u << i)) n++;
    return n;
}

int task_pid_capacity(void)
{
    return PID_MAX;
}

int task_slot_used(void)
{
    int i, n = 0;

    for (i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED) n++;
    return n;
}

int task_slot_capacity(void)
{
    return TASK_MAX;
}

static void task_note_switch(struct task_struct *next)
{
    switch_total++;
    if ((switch_total % TASK_TRACE_DIV) == 0) {
        switch_trace[switch_i] = (u32)next->pid;
        switch_i = (switch_i + 1) % TASK_TRACE_N;
        trace_total++;
    }
}

/*
 * P11.9：CFS-lite 加权公平选择。priority 0..3 对应权重 8/4/2/1，
 * 每个 PIT tick 给运行任务的 vruntime 增加 64/weight；选择 READY 中
 * vruntime 最小者。权重全为正且最小为 1，低优先级任务仍会轮到，
 * 不会被 FIFO 式地永久饿死；min_vruntime 防止睡眠任务醒来后一次抢回
 * 整段历史 CPU。
 * P11.4：只要存在非 idle READY，idle 就不参与本轮选择；只有全系统都
 * 没有可运行任务时才选 idle，让它在 sti; hlt 上真正停机。
 */
static struct task_struct *task_pick_next_impl(struct task_struct *prev)
{
    struct task_struct *best = 0;
    struct task_struct *idle_best = 0;
    struct task_struct *rt_task;
    u64 best_v = 0;
    int start = 0;
    int i;

    task_vruntime_refresh_min();

    /*
     * P13.1：RT 类走独立就绪队列，选队头是 O(1)（4 位 mask 只做常数扫描）。
     * RT 队列非空时不再进入 fair 扫描：RT 优先于 fair，fair 只在没有
     * 可运行 RT 时拿剩余 CPU。
     */
    if (rt_mask) {
        int prio = rt_lowest_priority();
        if (prio >= 0) {
            rt_task = rt_pop_head(prio);
            if (rt_task) {
                rt_task->slice_left = TASK_SLICE_TICKS;
                if (rt_task->rt_budget_cycles && !rt_task->rt_budget_last_tsc)
                    rt_task->rt_budget_last_tsc = tsc_read32();
                return rt_task;
            }
        }
    }

    for (i = 0; i < TASK_MAX; i++) {
        if (&tasks[i] == prev) {
            start = i + 1;
            break;
        }
    }

    for (i = 0; i < TASK_MAX; i++) {
        int idx = (start + i) % TASK_MAX;
        struct task_struct *t = &tasks[idx];
        if (t->state != TASK_READY) continue;
        if (t == idle_task) {
            if (!idle_best) idle_best = t;
            continue;
        }
        /* RT 只从 rt_mask 队列选；这里跳过，避免两套选择规则打架。 */
        if (t->rt) continue;
        if (!best || t->vruntime < best_v) {
            best = t;
            best_v = t->vruntime;
        }
    }
    /*
     * B 键演示：P11.4 的规则会让 idle 永远轮不到，proc_idle() 也就永远
     * 消费不到 idle_block_req。这里只放行一次，让 idle 真正执行
     * task_block()，状态行才会出现 BLOCKED / 3id B。
     */
    if (idle_demo_pick && idle_best) {
        idle_demo_pick = 0;
        idle_best->slice_left = TASK_SLICE_TICKS;
        return idle_best;
    }
    if (best) {
        best->slice_left = TASK_SLICE_TICKS;
        return best;
    }
    if (idle_best) {
        idle_best->slice_left = TASK_SLICE_TICKS;
        return idle_best;
    }
    return 0;
}

/*
 * P13.1：只在 RT bench 构建里给选任务路径加 TSC 探头。普通构建没有
 * 这层包装开销，pick_* 统计恒为 0。
 */
static struct task_struct *task_pick_next(struct task_struct *prev)
{
#ifdef AEVUM_RT_BENCH
    u32 t0 = tsc_read32();
#endif
    struct task_struct *next = task_pick_next_impl(prev);
#ifdef AEVUM_RT_BENCH
    u32 t1 = tsc_read32();
    u32 dt = t1 - t0;
    pick_calls++;
    pick_cycles_total += dt;
    if (dt > pick_cycles_max) pick_cycles_max = dt;
#endif
    return next;
}

/*
 * RT-2：CBS 账本。预算按 TSC 周期记；只有 RUNNING 的 RT 任务消费预算。
 * 首次进入或刚补满时 last_tsc=0，由第一次记账/选中时建立起点。
 */
static void task_rt_budget_reset(struct task_struct *t)
{
    if (!t || !t->rt || !t->rt_budget_cycles) return;
    t->rt_budget_left = t->rt_budget_cycles;
    t->rt_budget_last_tsc = 0;
    t->rt_throttled = 0;
    t->rt_throttle_until = 0;
}

static int task_rt_budget_charge(struct task_struct *t, u32 now)
{
    u32 elapsed;

    if (!t || !t->rt || !t->rt_budget_cycles || t->state != TASK_RUNNING)
        return 0;
    if (!t->rt_budget_last_tsc) {
        t->rt_budget_last_tsc = now;
        return 0;
    }
    elapsed = now - t->rt_budget_last_tsc;
    t->rt_budget_last_tsc = now;
    if (t->rt_preempted) t->rt_after_cut_cycles += elapsed;
    if (elapsed >= t->rt_budget_left) {
        t->rt_budget_left = 0;
        if (!t->rt_throttled) {
            t->rt_throttled = 1;
            t->rt_throttle_until = tick_total +
                                   (t->rt_period_ticks ? t->rt_period_ticks : 1u);
            /* 预算耗尽的 RT 不再留在就绪队列，否则会被反复选中。 */
            rt_dequeue(t);
            rt_budget_overrun_total++;
            rt_budget_throttle_total++;
        }
        return 1;
    }
    t->rt_budget_left -= elapsed;
    return 0;
}

/*
 * RT-1 第一刀：唤醒抢占。只置 need_resched，不直接切栈；
 * IRQ0 出口看到标记后绕过 P11.5 的 tick_defer，再走原 task_pick_next。
 * RT 类任务优先于 fair；普通任务只在高优先级唤醒时抢占。
 */
static void task_note_wake(struct task_struct *t)
{
    struct task_struct *cur = current_task;

    if (!t || !cur || cur->state != TASK_RUNNING) return;
    if (t == cur || t == idle_task) return;
    /* RT-2：一次唤醒就是新周期，补满预算并解除 throttle。 */
    if (t->rt) task_rt_budget_reset(t);
    /*
     * P13.1：RT 唤醒优先于 fair；高优先级 RT 抢占低优先级 RT；
     * 同优先级 RT 不互相抢占，按 FIFO 排到队尾。
     */
    if (cur == idle_task) {
        need_resched = 1;
        wake_resched_total++;
    } else if (t->rt) {
        if (!cur->rt || t->priority < cur->priority) {
            need_resched = 1;
            wake_resched_total++;
        }
    } else if (!cur->rt && t->priority < cur->priority) {
        need_resched = 1;
        wake_resched_total++;
    }
}

/* P11.1：每个 PIT tick 扫描一次定时阻塞；tick 是 32 位，比较用有符号差值防回绕。 */
static void task_wake_expired(u32 now)
{
    int i;

    for (i = 0; i < TASK_MAX; i++) {
        struct task_struct *t = &tasks[i];
        if (t->state != TASK_BLOCKED || !t->wake_tick) continue;
        if ((int)(now - t->wake_tick) < 0) continue;
        t->wake_tick = 0;
        t->waitq = 0;
        t->wait_ticket = 0;
        task_vruntime_wake(t);
        task_note_wake(t);
        task_make_ready(t);
        if (t == rt_probe_task) rt_probe_wake_tsc = tsc_read32();
        timeout_wake_total++;
    }
}

static void task_reap(void)
{
    int i;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_ZOMBIE && !tasks[i].proc &&
            !tasks[i].kthread) {
            pid_free(tasks[i].pid);
            if (tasks[i].user) address_space_release(tasks[i].space);
            task_zero(&tasks[i]);
        }
    }
}

int task_current_is_kthread(void)
{
    return current_task && current_task->kthread;
}

void task_set_kthread_wait(int tid)
{
    if (current_task) current_task->kthread_wait_tid = tid;
}

/* kthread 已经真正 ZOMBIE：只唤醒明确在等这个 TID 的 joiner。
 * wait_tid 保留到 kthread_join() 真正取走退出码并回收后再清，
 * 否则兜底 reaper 可能抢在已唤醒的 joiner 前把目标收掉。 */
void task_wake_kthread_waiters(int tid)
{
    int i;

    if (tid <= 0) return;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_BLOCKED &&
            tasks[i].kthread_wait_tid == tid) {
            tasks[i].wake_tick = 0;
            tasks[i].waitq = 0;
            tasks[i].wait_ticket = 0;
            task_vruntime_wake(&tasks[i]);
            task_note_wake(&tasks[i]);
            task_make_ready(&tasks[i]);
        }
    }
}

/*
 * kthread_join 专用回收：先释放动态栈，再归还 PID 和任务槽。
 * 与静态内核任务不同，kthread 不能走 task_reap() 的即时回收，
 * 否则 joiner 永远看不到 ZOMBIE。
 */
int task_reap_kthread(int tid)
{
    int i;

    if (tid <= 0) return -1;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].pid == tid && tasks[i].kthread &&
            tasks[i].state == TASK_ZOMBIE) {
            if (&tasks[i] == current_task) return -1;
            kernel_stack_free(tasks[i].stack_top);
            pid_free(tasks[i].pid);
            task_zero(&tasks[i]);
            return 0;
        }
    }
    return -1;
}

/*
 * 未 join 的 kthread 兜底：超过 grace 个 PIT tick 仍没人等这个 TID，
 * 就归还动态栈、PID 和任务槽。有人在 wait_tid 上等时绝不抢收，
 * 让正常 join 先拿到退出码。
 */
int task_reap_unjoined_kthread(u32 now, u32 grace)
{
    int i, j, n = 0;

    for (i = 0; i < TASK_MAX; i++) {
        struct task_struct *t = &tasks[i];
        int waited = 0;

        if (t->state != TASK_ZOMBIE || !t->kthread) continue;
        if (t == current_task) continue;
        if (now - t->zombie_ticks < grace) continue;

        for (j = 0; j < TASK_MAX; j++) {
            if (tasks[j].state != TASK_UNUSED &&
                tasks[j].kthread_wait_tid == t->pid) {
                waited = 1;
                break;
            }
        }
        if (waited) continue;

        kernel_stack_free(t->stack_top);
        pid_free(t->pid);
        task_zero(t);
        n++;
    }
    return n;
}

/* 用户 ZOMBIE 由 process_wait/reaper 调用；这里只清任务槽，不释放 PID/AS。 */
int task_release_slot(int pid)
{
    int i;

    if (pid <= 0) return -1;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].pid == pid && tasks[i].state == TASK_ZOMBIE) {
            if (&tasks[i] == current_task) return -1;
            task_zero(&tasks[i]);
            return 0;
        }
    }
    return -1;
}

int task_process_live_threads(const struct process *proc)
{
    int i;
    int n = 0;

    if (!proc) return 0;
    for (i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].proc == proc &&
            tasks[i].state != TASK_ZOMBIE)
            n++;
    return n;
}

int task_process_thread_count(const struct process *proc)
{
    int i;
    int n = 0;

    if (!proc) return 0;
    for (i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED && tasks[i].proc == proc) n++;
    return n;
}

int task_snapshot_process(const struct process *proc, struct task_info *out)
{
    int i;
    int fallback = -1;

    if (!out || !proc) return 0;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_UNUSED || tasks[i].proc != proc) continue;
        if (tasks[i].state != TASK_ZOMBIE) {
            task_fill_info(&tasks[i], out);
            return 1;
        }
        if (fallback < 0) fallback = i;
    }
    if (fallback >= 0) {
        task_fill_info(&tasks[fallback], out);
        return 1;
    }
    return 0;
}

int task_thread_snapshot(const struct process *proc, int tid,
                         struct task_info *out)
{
    int i;

    if (!out || !proc || tid <= 0) return 0;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].proc == proc &&
            tasks[i].pid == tid) {
            task_fill_info(&tasks[i], out);
            return 1;
        }
    }
    return 0;
}

void task_set_current_exit_code(u32 code)
{
    if (current_task) current_task->exit_code = code & 0xFFu;
}

int task_detach_thread(struct process *proc, int tid)
{
    int i;

    if (!proc || tid <= 0) return -1;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].proc == proc &&
            tasks[i].pid == tid) {
            tasks[i].detached = 1;
            return 0;
        }
    }
    return -1;
}

/*
 * detached 线程已经退出，但当前任务可能正踩在它的内核栈上；只回收
 * 非 current 的 ZOMBIE。当前任务的栈会在下一次切换时由这里回收。
 */
static void task_reap_detached(void)
{
    int i;

    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_ZOMBIE || !tasks[i].proc ||
            !tasks[i].detached || &tasks[i] == current_task)
            continue;
        kernel_stack_free(tasks[i].stack_top);
        pid_free(tasks[i].pid);
        task_zero(&tasks[i]);
    }
}

int task_reap_thread(struct process *proc, int tid)
{
    int i;

    if (!proc || tid <= 0) return -1;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].proc == proc &&
            tasks[i].pid == tid && tasks[i].state == TASK_ZOMBIE) {
            if (&tasks[i] == current_task) return -1;
            kernel_stack_free(tasks[i].stack_top);
            pid_free(tasks[i].pid);
            task_zero(&tasks[i]);
            return 0;
        }
    }
    return -1;
}

int task_release_process_threads(struct process *proc)
{
    int i;

    if (!proc) return -1;
    /*
     * 整组先确认、后释放：线程组里只要还有一个槽没到
     * TASK_ZOMBIE（或就是 current_task 本身），立即整体放弃，
     * 不进入释放循环——否则先放的槽已清空、后面的槽放不掉，
     * 会留下半释放的线程组。
     */
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_UNUSED || tasks[i].proc != proc) continue;
        if (&tasks[i] == current_task) return -1;
        if (tasks[i].state != TASK_ZOMBIE) return -1;
    }
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_UNUSED || tasks[i].proc != proc) continue;
        kernel_stack_free(tasks[i].stack_top);
        pid_free(tasks[i].pid);
        task_zero(&tasks[i]);
    }
    /*
     * 零槽也返回成功：调用方 process_reap() 已经确认线程组没有活任务。
     * 没有槽可释放说明任务层已经清完，不能把它当失败卡住 PCB/AS 回收。
     */
    return 0;
}

void task_terminate_threads(struct process *proc, int except_tid)
{
    int i;

    if (!proc) return;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_UNUSED || tasks[i].proc != proc) continue;
        if (tasks[i].pid == except_tid || tasks[i].state == TASK_ZOMBIE) continue;
        tasks[i].exit_code = 0;
        tasks[i].wake_tick = 0;
        tasks[i].waitq = 0;
        tasks[i].wait_ticket = 0;
        rt_dequeue(&tasks[i]);
        tasks[i].state = TASK_ZOMBIE;
    }
}

/*
 * 唯一调度入口：IRQ0、int 0x40 都从这里切栈。
 * 返回下一个任务的帧指针；loader.asm 恢复该帧并 iret。
 */
u32 task_schedule_irq(struct irq_frame *frame)
{
    struct task_struct *prev = current_task;
    struct task_struct *next;
    u32 reason = sched_reason;
    int prev_reaped = 0;
    int tick_defer = 0;

    if (!prev || !frame) {
        for (;;) __asm__ __volatile__("cli; hlt");
    }

    if (frame->vector == 0x30) {
        int last;

        /*
         * P13.2：LAPIC one-shot 1ms 伺服。不推进 10ms tick，不做
         * vruntime/slice 记账；只在最后事件做强制的 RT 预算摘链。
         */
        lapic_timer_eoi();
        reason = SCHED_TICK;
        last = interrupt_lapic_event();
        if (prev->state == TASK_RUNNING && prev->rt &&
            prev->rt_budget_cycles) {
            if (last) {
                (void)task_rt_budget_force_expire();
                need_resched = 1;
            } else if (task_rt_budget_expire()) {
                interrupt_lapic_note_early();
                interrupt_disarm_rt_budget();
                need_resched = 1;
            }
            tick_defer = need_resched ? 0 : 1;
        } else {
            interrupt_disarm_rt_budget();
            tick_defer = 1;
        }
    } else if (frame->vector == 0x20) {
        interrupt_eoi();
        reason = SCHED_TICK;
        /*
         * RT-2：PIT 通道 0 在预算到期时切到 one-shot 子事件。该次 IRQ 不
         * 推进 10ms tick，只记 CBS 账；处理完由 interrupt.c 再装到下一个
         * 10ms tick，tick 到达时恢复周期模式。
         */
        if (interrupt_timer_event()) {
            if (prev->state == TASK_RUNNING &&
                task_rt_budget_expire()) {
                need_resched = 1;
            }
        } else {
            tick_total++;
            task_rt_budget_tick(tick_total);
            task_wake_expired(tick_total);
            if (prev->state == TASK_RUNNING) {
                if (task_rt_budget_charge(prev, tsc_read32()))
                    need_resched = 1;
                prev->cpu_ticks++;
                prev->vruntime += WEIGHT_QUANTUM / task_weight(prev);
                /*
                 * P13.1：RT 不再消费 4 tick 固定片。它一直跑到 block/yield、
                 * 预算耗尽或被更高优先级 RT 抢占；fair 仍按原 slice_left。
                 */
                if (!prev->rt && prev->slice_left > 0) prev->slice_left--;
                if (prev->rt) {
                    tick_defer = need_resched ? 0 : 1;
                } else if (prev->slice_left > 0) {
                    if (need_resched) {
                        /* RT-1/RT-2：有唤醒或超预算抢占请求，片未尽也重选。 */
                        tick_defer = 0;
                    } else {
                        tick_defer_total++;
                        tick_defer = 1;
                    }
                } else {
                    slice_expire_total++;
                }
            }
        }
    }
    /* RT-2：yield/block 等非 tick 退出也把已消费预算结清。 */
    if (reason != SCHED_TICK && prev->state == TASK_RUNNING)
        (void)task_rt_budget_charge(prev, tsc_read32());
    prev->ctx.esp = (u32)frame;
    sched_failed = 0;

    /* P11.5：片未尽就不重选；RT-1 的 need_resched 是唯一旁路。 */
    if (reason == SCHED_TICK && tick_defer)
        return (u32)frame;
    need_resched = 0;

    if (reason == SCHED_START) {
        next = start_target;
        if (!next || next->state != TASK_READY) {
            sched_failed = 1;
            for (;;) __asm__ __volatile__("cli; hlt");
        }
        task_make_running(next);
        current_task = next;
        task_note_switch(next);
        address_space_switch(next->space);
        tss_set_esp0(next->stack_top);
        fpu_save((void *)prev->fpu_state);
        fpu_restore((void *)next->fpu_state);
        sched_reason = SCHED_YIELD;
        return next->ctx.esp;
    }

    if (prev->state == TASK_RUNNING) {
        if (reason == SCHED_EXIT) {
            rt_dequeue(prev);
            prev->state = TASK_ZOMBIE;
            prev->zombie_ticks = tick_total;
        } else if (reason == SCHED_BLOCK) {
            rt_dequeue(prev);
            prev->state = TASK_BLOCKED;
        } else {
            task_make_ready(prev);       /* yield 或 PIT 时间片到期 */
        }
    }

    /* 内核任务死亡时仍走 family 树；用户进程的父子关系归 process 表。 */
    if (prev->state == TASK_ZOMBIE) {
        if (!prev->proc) family_exit(prev->pid);
        /*
         * ZOMBIE 会在 task_reap() 里被 task_zero()，之后 fpu_state 会变成 0；
         * 必须在清槽前保存自己的 FPU 状态，不能让 task_zero 后的指针进入 fxsave。
         */
        fpu_save((void *)prev->fpu_state);
        /*
         * P8：动态内核线程已经真正 ZOMBIE，只在此时唤醒等它的 joiner。
         * 与 P7.5 相同，提前唤醒会让等待者重查后再次睡死。
         */
        if (prev->kthread) task_wake_kthread_waiters(prev->pid);
        /*
         * P7.5：任务已经真的变成 ZOMBIE，此时才唤醒 join 等待者。
         * 不能在 process_thread_exit_current() 里提前唤醒：目标可能还是
         * RUNNING，等待者会重查后再次阻塞，形成 lost wake。
         */
        process_notify_task_zombie(prev->proc);
        prev_reaped = 1;
    }

    task_reap();
    task_reap_detached();
    kthread_reap_unjoined();
    process_reap_orphans();
    next = task_pick_next(prev);

    if (!next) {
        if (reason == SCHED_EXIT)
            for (;;) __asm__ __volatile__("cli; hlt");
        if (reason == SCHED_BLOCK) {
            task_make_running(prev);
            sched_failed = 1;
        } else {
            task_make_running(prev);
        }
        next = prev;
    }

    if (next != prev) {
        if (!prev_reaped) prev->switches++;
        task_make_running(next);
        if (reason == SCHED_TICK) preempt_total++;
        task_note_switch(next);
    } else {
        /*
         * 选中自己时也必须恢复 RUNNING：否则一次 yield 会把当前任务
         * 永久留在 READY，PIT 不再给它计费，vruntime 也不再增长。
         */
        task_make_running(next);
    }
    if (next != prev) {
        if (!prev_reaped) fpu_save((void *)prev->fpu_state);
        fpu_restore((void *)next->fpu_state);
    }
    current_task = next;
    address_space_switch(next->space);
    tss_set_esp0(next->stack_top);
    sched_reason = SCHED_YIELD;
    return next->ctx.esp;
}

/*
 * 当前任务状态的唯一写入口。状态不是随便赋值：
 * READY/BLOCKED/ZOMBIE 都必须立刻进入调度器，否则 RUNNING 位会失真。
 */
const char *task_set_current_state(int state)
{
    struct task_struct *prev = current_task;

    if (!prev || prev->state != TASK_RUNNING) return "state";
    if (state == TASK_RUNNING) return 0;
    prev->wake_tick = 0;
    prev->waitq = 0;
    prev->wait_ticket = 0;

    if (state == TASK_READY) {
        sched_reason = SCHED_YIELD;
    } else if (state == TASK_BLOCKED) {
        sched_reason = SCHED_BLOCK;
    } else if (state == TASK_ZOMBIE) {
        sched_reason = SCHED_EXIT;
    } else {
        return "state";
    }

    sched_failed = 0;
    __asm__ __volatile__("int $0x40");

    if (state == TASK_ZOMBIE)
        for (;;) __asm__ __volatile__("cli; hlt");

    return sched_failed ? "schedule" : 0;
}

void task_exit(void)
{
    (void)task_set_current_state(TASK_ZOMBIE);
    for (;;) __asm__ __volatile__("cli; hlt");
}

const char *task_block(void)
{
    return task_set_current_state(TASK_BLOCKED);
}

const char *task_wake(int pid)
{
    int i;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].pid == pid && tasks[i].state == TASK_BLOCKED) {
            tasks[i].wake_tick = 0;
            tasks[i].waitq = 0;
            tasks[i].wait_ticket = 0;
            task_vruntime_wake(&tasks[i]);
            task_note_wake(&tasks[i]);
            task_make_ready(&tasks[i]);
            return 0;
        }
    }
    return "pid";
}

/*
 * P11.7/P11.8：条件不满足时检查 pending signal。只 peek，不消费；
 * 返回用户态前由 process_signal_deliver_async() 统一消费/投递。
 */
static int task_block_signal(void)
{
    if (!process_signal_peek_current()) return 0;
    eintr_total++;
    return 1;
}

int task_block_until(task_block_check_t ready, void *ctx)
{
    u32 flags;

    if (!current_task || current_task->state != TASK_RUNNING) return -1;
    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    current_task->wake_tick = 0;
    current_task->waitq = 0;
    current_task->wait_ticket = 0;
    if (ready && ready(ctx)) {
        __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
        return 0;
    }
    if (task_block_signal()) {
        __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
        return 2;
    }

    /*
     * 先写 BLOCKED 再进门，调度器不会再把当前任务当成 RUNNING 重新放回。
     * 从 cli 到 int 0x40 之间没有 PIT，也没有其他任务能运行，因此
     * “重查条件”和“真正阻塞”是一个不可分割的窗口。
     */
    current_task->state = TASK_BLOCKED;
    sched_reason = SCHED_BLOCK;
    sched_failed = 0;
    __asm__ __volatile__("int $0x40");
    __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
    return sched_failed ? -1 : 1;
}

/*
 * P11.1：定时阻塞。deadline 在进入时固定，不因虚假唤醒重置；条件优先于超时，
 * 同一 tick 内子进程退出时 wait_timeout 必须返回 status，而不是误报 timeout。
 */
int task_block_until_timeout(task_block_check_t ready, void *ctx, u32 ticks)
{
    u32 flags;
    u32 deadline;

    if (!current_task || current_task->state != TASK_RUNNING) return -1;
    if (ticks > 0x7FFFFFFFu) ticks = 0x7FFFFFFFu;
    if (!ticks) return (ready && ready(ctx)) ? 0 : 1;
    deadline = tick_total + ticks;
    if (!deadline) deadline = 1;

    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    current_task->waitq = 0;
    current_task->wait_ticket = 0;
    for (;;) {
        if (ready && ready(ctx)) {
            current_task->wake_tick = 0;
            __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
            return 0;
        }
        if ((int)(tick_total - deadline) >= 0) {
            current_task->wake_tick = 0;
            __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
            return 1;
        }
        if (task_block_signal()) {
            current_task->wake_tick = 0;
            current_task->waitq = 0;
            current_task->wait_ticket = 0;
            __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
            return 2;
        }
        current_task->state = TASK_BLOCKED;
        current_task->wake_tick = deadline;
        sched_reason = SCHED_BLOCK;
        sched_failed = 0;
        __asm__ __volatile__("int $0x40");
        if (sched_failed) {
            current_task->wake_tick = 0;
            __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
            return -1;
        }
    }
}

int task_sleep_ticks(u32 ticks)
{
    int rc;

    if (ticks > 0x7FFFFFFFu) ticks = 0x7FFFFFFFu;
    if (!ticks) return 0;
    sleep_block_total++;
    rc = task_block_until_timeout(0, 0, ticks);
    if (rc == 1) {
        sleep_wake_total++;
        return 0;
    }
    if (rc == 2) return -3;     /* P11.7：pending signal -> EINTR */
    return rc < 0 ? -1 : 0;
}

static u32 wait_queue_depth(struct wait_queue *q)
{
    int i;
    u32 n = 0;

    for (i = 0; i < TASK_MAX; i++)
        if (tasks[i].state == TASK_BLOCKED && tasks[i].waitq == q) n++;
    return n;
}

/* P11.6：0 保留为“未排队”，票据单调递增；回绕时跳过 0。 */
static u32 wait_ticket_alloc(struct wait_queue *q)
{
    u32 ticket = q->next_ticket++;

    if (!ticket) {
        q->next_ticket = 2;
        ticket = 1;
    }
    return ticket;
}

void wait_queue_init(struct wait_queue *q)
{
    if (!q) return;
    q->next_ticket = 1;
    q->wakes = 0;
}

/*
 * P11.6：定向唤醒。只把票据最小的 BLOCKED 成员改 READY，不清 wait_ticket；
 * 被唤醒者若重查条件后仍不满足，会带着原票据重新入队，保持 FIFO 位置。
 * 真正返回调用方或超时/退出时才清票据。
 */
void wait_queue_wake_one(struct wait_queue *q)
{
    u32 flags;
    struct task_struct *best = 0;
    u32 best_ticket = 0;
    int i;

    if (!q) return;
    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    for (i = 0; i < TASK_MAX; i++) {
        struct task_struct *t = &tasks[i];
        if (t->state != TASK_BLOCKED || t->waitq != q) continue;
        if (!best || (int)(t->wait_ticket - best_ticket) < 0) {
            best = t;
            best_ticket = t->wait_ticket;
        }
    }
    if (best) {
        best->wake_tick = 0;
        best->waitq = 0;
        task_vruntime_wake(best);
        task_note_wake(best);
        task_make_ready(best);
        q->wakes++;
        wq_wake_one_total++;
    }
    __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
}

/*
 * P11.3/P11.6：广播唤醒。关闭/EOF 这类终止条件仍一次放开所有等待者；
 * 票据在这里一并清掉，因为等待者已经离开该对象。
 */
void wait_queue_wake_all(struct wait_queue *q)
{
    u32 flags;
    int i, woke = 0;

    if (!q) return;
    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].waitq == q) {
            tasks[i].wake_tick = 0;
            tasks[i].waitq = 0;
            tasks[i].wait_ticket = 0;
            task_vruntime_wake(&tasks[i]);
            task_note_wake(&tasks[i]);
            task_make_ready(&tasks[i]);
            woke = 1;
        }
    }
    if (woke) {
        q->wakes++;
        wq_wake_all_total++;
    }
    __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
}

int task_block_on(struct wait_queue *q, task_block_check_t ready,
                  void *ctx, u32 ticks)
{
    u32 flags;
    u32 deadline;
    int blocked = 0;

    if (!q || !current_task || current_task->state != TASK_RUNNING) return -1;
    if (ticks > 0x7FFFFFFFu) ticks = 0x7FFFFFFFu;
    if (!ticks) return (ready && ready(ctx)) ? 0 : 1;
    deadline = tick_total + ticks;
    if (!deadline) deadline = 1;

    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    for (;;) {
        if (ready && ready(ctx)) {
            current_task->wake_tick = 0;
            current_task->waitq = 0;
            current_task->wait_ticket = 0;
            if (blocked) io_wake_total++;
            __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
            return 0;
        }
        if ((int)(tick_total - deadline) >= 0) {
            current_task->wake_tick = 0;
            current_task->waitq = 0;
            current_task->wait_ticket = 0;
            if (blocked) io_timeout_total++;
            __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
            return 1;
        }
        if (task_block_signal()) {
            current_task->wake_tick = 0;
            current_task->waitq = 0;
            current_task->wait_ticket = 0;
            __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
            return 2;
        }
        current_task->state = TASK_BLOCKED;
        current_task->wake_tick = deadline;
        /*
         * P11.6：首次入队分配票据；被定向唤醒但输掉条件重查时保留原票据，
         * 不重新排到队尾。深度在成员真正 BLOCKED 之后再统计。
         */
        if (!current_task->waitq) {
            if (!current_task->wait_ticket)
                current_task->wait_ticket = wait_ticket_alloc(q);
            current_task->waitq = q;
            {
                u32 depth = wait_queue_depth(q);
                if (depth > wq_max_depth) wq_max_depth = depth;
            }
        }
        if (!blocked) {
            blocked = 1;
            io_block_total++;
        }
        sched_reason = SCHED_BLOCK;
        sched_failed = 0;
        __asm__ __volatile__("int $0x40");
        current_task->waitq = 0;
        if (sched_failed) {
            current_task->wake_tick = 0;
            current_task->wait_ticket = 0;
            __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
            return -1;
        }
    }
}

void task_io_stats(u32 *blocks, u32 *wakes, u32 *timeouts)
{
    if (blocks) *blocks = io_block_total;
    if (wakes) *wakes = io_wake_total;
    if (timeouts) *timeouts = io_timeout_total;
}

void task_wq_stats(u32 *blocks, u32 *wake_one, u32 *wake_all, u32 *max_depth)
{
    if (blocks) *blocks = io_block_total;
    if (wake_one) *wake_one = wq_wake_one_total;
    if (wake_all) *wake_all = wq_wake_all_total;
    if (max_depth) *max_depth = wq_max_depth;
}

u32 task_eintr_total(void)
{
    return eintr_total;
}

void task_set_idle_task(struct task_struct *task)
{
    idle_task = task;
}

void task_force_idle_demo(void)
{
    idle_demo_pick = 1;
}

void task_note_idle_hlt(void)
{
    idle_hlt_total++;
}

u32 task_idle_hlt_total(void)
{
    return idle_hlt_total;
}

void task_slice_stats(u32 *quantum, u32 *expires, u32 *defers)
{
    if (quantum) *quantum = TASK_SLICE_TICKS;
    if (expires) *expires = slice_expire_total;
    if (defers) *defers = tick_defer_total;
}

void task_timer_stats(u32 *sleep_blocks, u32 *sleep_wakes, u32 *timeouts)
{
    if (sleep_blocks) *sleep_blocks = sleep_block_total;
    if (sleep_wakes) *sleep_wakes = sleep_wake_total;
    if (timeouts) *timeouts = timeout_wake_total;
}

void task_wake_process(struct process *proc)
{
    int i;
    if (!proc) return;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].proc == proc && tasks[i].state == TASK_BLOCKED) {
            tasks[i].wake_tick = 0;
            tasks[i].waitq = 0;
            tasks[i].wait_ticket = 0;
            task_vruntime_wake(&tasks[i]);
            task_note_wake(&tasks[i]);
            task_make_ready(&tasks[i]);
        }
    }
}

const char *task_yield(void)
{
    return task_set_current_state(TASK_READY);
}

/*
 * 等待型 yield：wait/join 的轮询必须在切走前记一次 vruntime，否则父线程
 * 会一直以最低 vruntime 被反复选中，把子任务饿死。普通 yield 不记账，
 * 让演示里的长 yield 窗口仍能快速推进。
 */
const char *task_yield_wait(void)
{
    if (!current_task || current_task->state != TASK_RUNNING) return "state";
    current_task->vruntime += WEIGHT_QUANTUM / task_weight(current_task);
    return task_yield();
}

void task_start(struct task_struct *first)
{
    start_target = first;
    sched_reason = SCHED_START;
    sched_failed = 0;
    __asm__ __volatile__("int $0x40");
    for (;;) __asm__ __volatile__("cli; hlt");
}

static u32 task_stack_used(struct task_struct *task)
{
    u32 *base = (u32 *)task->stack_base;
    int n = (int)((task->stack_top - task->stack_base) / 4);
    int i;

    for (i = 0; i < n; i++)
        if (base[i] != STACK_MAGIC) break;

    if (i >= n) return 32;
    return task->stack_top - (task->stack_base + (u32)i * 4u);
}

static void task_fill_info(struct task_struct *task, struct task_info *out)
{
    int i;

    out->pid = task->pid;
    out->tgid = task->tgid;
    out->state = task->state;
    out->priority = task->priority;
    for (i = 0; i <= TASK_NAME_MAX; i++) out->comm[i] = task->comm[i];
    out->stack_top = task->stack_top;
    out->stack_used = task_stack_used(task);
    out->switches = task->switches;
    out->cpu_ticks = task->cpu_ticks;
    out->exit_code = task->exit_code;
    out->detached = task->detached;
    out->kthread = task->kthread;
}

int task_snapshot(int slot, struct task_info *out)
{
    struct task_struct *task;

    if (!out || slot < 0 || slot >= TASK_MAX) return 0;
    task = &tasks[slot];
    if (task->state == TASK_UNUSED) return 0;
    task_fill_info(task, out);
    return 1;
}

int task_snapshot_pid(int pid, struct task_info *out)
{
    int i;

    if (!out) return 0;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid) {
            task_fill_info(&tasks[i], out);
            return 1;
        }
    }
    return 0;
}

int task_trace_len(void)
{
    if (trace_total > TASK_TRACE_N) return TASK_TRACE_N;
    return (int)trace_total;
}

u32 task_trace_pid(int index)
{
    int len = task_trace_len();
    int start;

    if (index < 0 || index >= len) return 0;
    start = (switch_i - len + TASK_TRACE_N) % TASK_TRACE_N;
    return switch_trace[(start + index) % TASK_TRACE_N];
}

u32 task_switch_total(void)
{
    return switch_total;
}

u32 task_preempt_total(void)
{
    return preempt_total;
}

u32 task_cpu_total(void)
{
    u32 total = 0;
    int i;

    /* 只汇总仍存活的用户任务，保证统计页各行百分比相加为 100%。 */
    for (i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED) total += tasks[i].cpu_ticks;
    return total;
}

u32 task_tick_total(void)
{
    return tick_total;
}

void task_rt_probe_set(struct task_struct *task)
{
    rt_probe_task = task;
    rt_probe_wake_tsc = 0;
}

void task_rt_probe_clear(void)
{
    rt_probe_wake_tsc = 0;
}

u32 task_rt_probe_wake_tsc(void)
{
    return rt_probe_wake_tsc;
}

/*
 * P13.1 验收探针：扫一遍 RT 队列，检查头尾/双向链/mask/state 是否自洽。
 * 只读，不参与调度。返回位图，0 表示一致。普通构建的 Q 页也用它。
 */
u32 task_rt_queue_audit(void)
{
    u8 seen[TASK_MAX];
    u32 err = 0;
    u16 queue_mask = 0;
    int i, prio;

    for (i = 0; i < TASK_MAX; i++) seen[i] = 0;

    for (prio = 0; prio < TASK_RT_PRIO_COUNT; prio++) {
        u8 cur = rt_head[prio];
        u8 prev = TASK_RT_NONE;
        int n = 0;

        if (cur != TASK_RT_NONE) queue_mask |= (u16)(1u << prio);
        while (cur != TASK_RT_NONE) {
            struct task_struct *t;

            if (cur >= TASK_MAX || n++ > TASK_MAX) {
                err |= 1u;
                break;
            }
            t = &tasks[cur];
            if (!t->rt || !t->rt_queued || t->state != TASK_READY ||
                t->rt_throttled) err |= 2u;
            if (t->rt_prio != (u8)prio) err |= 4u;
            if (t->rt_prev != prev) err |= 8u;
            if (seen[cur]) err |= 16u;
            seen[cur] = 1;
            if (t->rt_next == TASK_RT_NONE) {
                if (rt_tail[prio] != cur) err |= 32u;
            } else if (t->rt_next >= TASK_MAX) {
                err |= 1u;
                break;
            }
            prev = cur;
            cur = t->rt_next;
        }
        if (rt_head[prio] == TASK_RT_NONE &&
            rt_tail[prio] != TASK_RT_NONE) err |= 64u;
    }

    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].rt_queued && !seen[i]) err |= 128u;
        if (tasks[i].rt && tasks[i].state == TASK_READY &&
            !tasks[i].rt_queued && !tasks[i].rt_throttled) err |= 256u;
    }
    if ((u8)queue_mask != rt_mask) err |= 512u;
    return err;
}

u32 task_rt_queue_mask(void)
{
    return (u32)rt_mask;
}

int task_rt_queue_head(int prio)
{
    if (prio < 0 || prio >= TASK_RT_PRIO_COUNT) return -1;
    return rt_head[prio] == TASK_RT_NONE ? -1 : (int)rt_head[prio];
}

int task_rt_queue_tail(int prio)
{
    if (prio < 0 || prio >= TASK_RT_PRIO_COUNT) return -1;
    return rt_tail[prio] == TASK_RT_NONE ? -1 : (int)rt_tail[prio];
}

u32 task_rt_queue_count(int prio)
{
    u8 slot;
    u32 n = 0;

    if (prio < 0 || prio >= TASK_RT_PRIO_COUNT) return 0;
    slot = rt_head[prio];
    while (slot != TASK_RT_NONE && n < TASK_MAX) {
        n++;
        slot = tasks[slot].rt_next;
    }
    return n;
}

u32 task_rt_queue_total(void)
{
    u32 n = 0;
    int prio;

    for (prio = 0; prio < TASK_RT_PRIO_COUNT; prio++)
        n += task_rt_queue_count(prio);
    return n;
}

int task_set_rt_tid(int tid)
{
    struct task_struct *task = task_find_pid(tid);

    if (!task || task->state == TASK_ZOMBIE) return -1;
    task_set_rt(task);
    return 0;
}

u32 task_pick_cycles_max(void)
{
    return pick_cycles_max;
}

u32 task_pick_cycles_avg(void)
{
    return pick_calls ? (pick_cycles_total / pick_calls) : 0u;
}

u32 task_pick_cycles_count(void)
{
    return pick_calls;
}

void task_pick_cycles_reset(void)
{
    pick_cycles_max = 0;
    pick_cycles_total = 0;
    pick_calls = 0;
}

void task_set_rt(struct task_struct *task)
{
    if (!task) return;
    task->rt = 1;
    /* 已在 READY 的任务要立刻进入 RT 队列，不能等下一次调度才生效。 */
    if (task->state == TASK_READY) rt_enqueue(task);
}

void task_set_rt_budget(struct task_struct *task, u32 budget_cycles,
                        u32 period_ticks)
{
    if (!task || !task->rt || !budget_cycles) return;
    task->rt_budget_cycles = budget_cycles;
    task->rt_period_ticks = period_ticks ? period_ticks : 1u;
    task_rt_budget_reset(task);
    if (task->state == TASK_READY) rt_enqueue(task);
}

void task_rt_budget_begin(void)
{
    if (!current_task || !current_task->rt || !current_task->rt_budget_cycles)
        return;
    current_task->rt_budget_left = current_task->rt_budget_cycles;
    current_task->rt_budget_last_tsc = tsc_read32();
    current_task->rt_after_cut_cycles = 0;
    current_task->rt_preempted = 0;
    current_task->rt_throttled = 0;
    current_task->rt_throttle_until = 0;
}

void task_rt_budget_end(void)
{
    if (!current_task || !current_task->rt || !current_task->rt_budget_cycles)
        return;
    (void)task_rt_budget_charge(current_task, tsc_read32());
    /*
     * 控制工作已经结束；预算定时器若在本周期剩余部分触发，不应把
     * 打印/截止检查等尾部代码算成控制超预算。
     */
    interrupt_disarm_rt_budget();
    current_task->rt_budget_last_tsc = 0;
}

int task_rt_budget_expire(void)
{
    if (!current_task || current_task->state != TASK_RUNNING)
        return 0;
    return task_rt_budget_charge(current_task, tsc_read32());
}

int task_rt_budget_force_expire(void)
{
    if (!current_task || current_task->state != TASK_RUNNING ||
        !current_task->rt || !current_task->rt_budget_cycles)
        return 0;
    (void)task_rt_budget_charge(current_task, tsc_read32());
    if (!current_task->rt_preempted) {
        current_task->rt_preempted = 1;
        current_task->rt_after_cut_cycles = 0;
        rt_preempt_total++;
    }
    if (!current_task->rt_throttled) {
        current_task->rt_budget_left = 0;
        current_task->rt_throttled = 1;
        current_task->rt_throttle_until = tick_total +
            (current_task->rt_period_ticks ? current_task->rt_period_ticks : 1u);
        rt_dequeue(current_task);
        rt_budget_overrun_total++;
        rt_budget_throttle_total++;
    }
    return 1;
}

void task_rt_budget_tick(u32 now_tick)
{
    int i;

    for (i = 0; i < TASK_MAX; i++) {
        struct task_struct *t = &tasks[i];

        if (!t->rt || !t->rt_budget_cycles || !t->rt_throttled) continue;
        if ((int)(now_tick - t->rt_throttle_until) >= 0) {
            t->rt_budget_left = t->rt_budget_cycles;
            t->rt_budget_last_tsc = 0;
            t->rt_throttled = 0;
            t->rt_throttle_until = 0;
            /* 补满后如果任务已 READY，重新进入 RT 队列并触发重选。 */
            if (t->state == TASK_READY) rt_enqueue(t);
            need_resched = 1;
        }
    }
}

u32 task_rt_budget_overrun_total(void)
{
    return rt_budget_overrun_total;
}

u32 task_rt_budget_throttle_total(void)
{
    return rt_budget_throttle_total;
}

u32 task_rt_budget_preempt_total(void)
{
    return rt_preempt_total;
}

u32 task_rt_budget_after_cut_cycles(void)
{
    return current_task ? current_task->rt_after_cut_cycles : 0u;
}

u32 task_wake_resched_total(void)
{
    return wake_resched_total;
}
