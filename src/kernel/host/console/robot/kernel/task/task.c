#include "kernel/task.h"
#include "kernel/task/family.h"
#include "kernel/task/stack.h"
#include "kernel/kthread.h"
#include "kernel/proc.h"
#include "arch/x86/interrupt.h"
#include "arch/x86/frame.h"
#include "arch/x86/tss.h"
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
    char comm[TASK_NAME_MAX + 1];
    u32  stack_base;
    u32  stack_top;
    u32  switches;
    u32  cpu_ticks;
    u32  exit_code;
    int  detached;  /* 1：线程退出后由调度器自动回收，不等待 join */
    int  kthread;   /* 1：P8 动态内核线程，ZOMBIE 由 join 或调度器兜底回收 */
    int  kthread_wait_tid; /* >0：当前任务正阻塞等待该 TID 的 kthread */
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

static volatile u32 sched_reason;
static volatile int sched_failed;

static u32 switch_trace[TASK_TRACE_N];
static int switch_i;
static u32 switch_total;
static u32 trace_total;
static u32 pid_map;
static u32 preempt_total;
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

static u32 task_weight(const struct task_struct *task)
{
    return (u32)(task->priority + 1);
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

static void task_zero(struct task_struct *task)
{
    int i;
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
    task->kthread_wait_tid = 0;
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
    boot_task.state = TASK_RUNNING;
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
    if (priority < 0 || priority > 3) return "priority";
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
    /* 新任务从当前虚拟时间起跑，不靠 0 值抢占已运行任务的时间片。 */
    task->vruntime = current_task ? current_task->vruntime : 0;
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
        task->state = TASK_READY;
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
    rc = task_create_common(1, name, user_eip, kernel_stack_top, space,
                            1, user_stack_top, proc, tid, 0, 0, 0, out);
    if (rc) return rc;

    task = *out;
    /* 线程入口从 eax 读 arg；首轮必须有机会跑，父线程随后 yield/join。 */
    sp = (u32 *)task->ctx.esp;
    sp[0] = arg;
    task->vruntime = 0;
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
    rc = task_create_common(1, name, frame->eip, kernel_stack_top, space,
                            1, frame->user_esp, proc, pid, 0, 0, 0, out);
    if (rc) return rc;
    task = *out;
    /* fork 子任务必须有机会先跑一次：父进程随后进入 wait，
     * 若子和父同 vruntime，调度器会一直选回父、饿死子。 */
    task->vruntime = 0;
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
 * 优先级加权公平选择：priority 0..3 对应权重 1..4，每次 PIT 给运行任务
 * 的 vruntime 增加 64/weight。选择 READY 中 vruntime 最小者；同值按
 * prev 之后的槽位轮转，避免严格最高优先把 render/idle 饿死。
 */
static struct task_struct *task_pick_next(struct task_struct *prev)
{
    struct task_struct *best = 0;
    u64 best_v = 0;
    int start = 0;
    int i;

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
        if (!best || t->vruntime < best_v) {
            best = t;
            best_v = t->vruntime;
        }
    }
    return best;
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
            tasks[i].state = TASK_READY;
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

    if (!prev || !frame) {
        for (;;) __asm__ __volatile__("cli; hlt");
    }

    if (frame->vector == 0x20) {
        interrupt_eoi();
        reason = SCHED_TICK;
        tick_total++;
        if (prev->state == TASK_RUNNING) {
            prev->cpu_ticks++;
            prev->vruntime += WEIGHT_QUANTUM / task_weight(prev);
        }
    }
    prev->ctx.esp = (u32)frame;
    sched_failed = 0;

    if (reason == SCHED_START) {
        next = start_target;
        if (!next || next->state != TASK_READY) {
            sched_failed = 1;
            for (;;) __asm__ __volatile__("cli; hlt");
        }
        next->state = TASK_RUNNING;
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
            prev->state = TASK_ZOMBIE;
            prev->zombie_ticks = tick_total;
        } else if (reason == SCHED_BLOCK)
            prev->state = TASK_BLOCKED;
        else
            prev->state = TASK_READY;       /* yield 或 PIT 时间片到期 */
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
            prev->state = TASK_RUNNING;
            sched_failed = 1;
        } else {
            prev->state = TASK_RUNNING;
        }
        next = prev;
    }

    if (next != prev) {
        if (!prev_reaped) prev->switches++;
        next->state = TASK_RUNNING;
        if (reason == SCHED_TICK) preempt_total++;
        task_note_switch(next);
    } else {
        /*
         * 选中自己时也必须恢复 RUNNING：否则一次 yield 会把当前任务
         * 永久留在 READY，PIT 不再给它计费，vruntime 也不再增长。
         */
        next->state = TASK_RUNNING;
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
            tasks[i].state = TASK_READY;
            return 0;
        }
    }
    return "pid";
}

int task_block_until(task_block_check_t ready, void *ctx)
{
    u32 flags;

    if (!current_task || current_task->state != TASK_RUNNING) return -1;
    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    if (ready && ready(ctx)) {
        __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
        return 0;
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

void task_wake_process(struct process *proc)
{
    int i;
    if (!proc) return;
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].proc == proc && tasks[i].state == TASK_BLOCKED)
            tasks[i].state = TASK_READY;
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
