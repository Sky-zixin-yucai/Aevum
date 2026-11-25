#include "kernel/proc.h"
#include "kernel/proc/fd.h"
#include "kernel/task.h"
#include "kernel/task/stack.h"
#include "kernel/mm/address_space.h"
#include "arch/x86/exception.h"
#include "arch/x86/frame.h"
#include "config/memory.h"

#define PROC_SIGFRAME_MAGIC 0x53494746u  /* "SIGF" */
#define PROC_EXEC_MAX_VEC 4
#define PROC_EXEC_STRING_MAX 32
#define PROC_REAP_SLOTS 2

/*
 * sigreturn 只保留信号恢复需要的算术/方向标志：
 * CF/PF/AF/ZF/SF/DF/OF 原样保留，bit1 恒为 1，IF 强制开启。
 * TF/IOPL/VM/NT/AC 一律清零：signal frame 位于用户栈，handler 可以
 * 任意篡改它；若原样恢复，伪造 IOPL=3 就能在 ring3 直接 in/out 端口
 * （TSS 无 iomap，IOPL>=CPL 即放行），清 IF 则屏蔽 PIT 挂死全系统。
 */
#define PROC_EFLAGS_USER_MASK 0x00000CD5u
#define PROC_EFLAGS_USER_FIXED 0x00000202u

struct proc_exec_args {
    u32 argc;
    u32 envc;
    char argv[PROC_EXEC_MAX_VEC][PROC_EXEC_STRING_MAX];
    char envp[PROC_EXEC_MAX_VEC][PROC_EXEC_STRING_MAX];
};

extern u8 user_program_start[];
extern u8 user_sigreturn_trampoline[];
extern u8 user_exec_start[];
extern u8 user_exec_end[];
extern u8 user_pipe_exec_start[];
extern u8 user_pipe_exec_end[];

struct proc_sigaction {
    u32 handler;
    u32 mask;
    u32 flags;
};

/*
 * signal frame 布局固定：低地址是 sigreturn trampoline 返回地址，随后是
 * 内核保存的原 iret 帧和通用寄存器。顺序必须与 process_signal_deliver()
 * 写入、process_sigreturn_current() 读回一致。
 */
struct proc_signal_frame {
    u32 eax, ebx, ecx, edx, esi, edi, ebp;
    u32 eip, cs, eflags, user_esp, ss;
    u32 sig, old_mask, magic;
};

struct proc_exit_record {
    u32 seq;
    int pid;
    int ppid;
    u32 exit_code;
    u32 term_sig;
    u32 image_id;
    u32 argc;
    u32 envc;
    char comm[PROC_NAME_MAX + 1];
};

struct process {
    int pid;
    int ppid;
    int state;
    u32 exit_code;
    char comm[PROC_NAME_MAX + 1];
    u32 stack_top;
    struct address_space *space;
    u32 pending;
    u32 last_sig;
    u32 term_sig;
    u32 sig_count;
    u32 sigchld_count;
    struct proc_sigaction sigactions[PROC_NSIG];
    u32 sig_mask;
    u32 sig_handled;
    u32 image_id;
    u32 exec_argc;
    u32 exec_envc;
    struct proc_fd_table fds;
};

static struct process procs[PROC_MAX];

/*
 * P10：PCB 被清零后，退出摘要留在这里。只用固定槽，避免为每个进程
 * 永久保留一份历史；4 槽同时覆盖 P4.2/P4.3 两次复用 PID 的证据。
 */
static struct proc_exit_record exit_ring[PROC_EXIT_HISTORY];
static u32 exit_seq;
static int exit_count;

/* P7.5 wait/join 的关中断重查上下文；函数定义放在进程表 helper 之后。 */
struct proc_wait_ctx {
    struct process *self;
    int pid;
};
struct proc_join_ctx {
    struct process *self;
    int tid;
};
static int proc_wait_ready(void *arg);
static int proc_join_ready(void *arg);

/* 最近一次 orphan/reparent 事件；V 页只读，不参与调度决策。 */
static struct proc_orphan_event last_orphan;
static u32 orphan_seq;

/* P7.5：只统计 wait/join 的阻塞与唤醒，不含 idle 的 task_block。 */
static u32 wait_block_total;
static u32 wait_wake_total;
static u32 join_block_total;
static u32 join_wake_total;

static void proc_reset_signals(struct process *p)
{
    int i;

    p->pending = 0;
    p->last_sig = 0;
    p->term_sig = 0;
    p->sig_count = 0;
    p->sigchld_count = 0;
    p->sig_mask = 0;
    p->sig_handled = 0;
    for (i = 0; i < PROC_NSIG; i++) {
        p->sigactions[i].handler = PROC_SIG_DFL;
        p->sigactions[i].mask = 0;
        p->sigactions[i].flags = 0;
    }
}

static void proc_zero(struct process *p)
{
    int i;
    proc_fd_table_close_all(&p->fds);
    p->pid = 0;
    p->ppid = 0;
    p->state = PROC_FREE;
    p->exit_code = 0;
    for (i = 0; i <= PROC_NAME_MAX; i++) p->comm[i] = 0;
    p->stack_top = 0;
    p->space = 0;
    p->image_id = 0;
    p->exec_argc = 0;
    p->exec_envc = 0;
    proc_fd_table_init(&p->fds);
    proc_reset_signals(p);
}

/* 只复制退出摘要，不带走 AS/fd/任务槽指针。 */
static void proc_exit_push(const struct process *p)
{
    struct proc_exit_record *r;
    int i;

    if (!p) return;
    r = &exit_ring[exit_seq % PROC_EXIT_HISTORY];
    exit_seq++;
    r->seq = exit_seq;
    r->pid = p->pid;
    r->ppid = p->ppid;
    r->exit_code = p->exit_code;
    r->term_sig = p->term_sig;
    r->image_id = p->image_id;
    r->argc = p->exec_argc;
    r->envc = p->exec_envc;
    for (i = 0; i <= PROC_NAME_MAX && p->comm[i]; i++) r->comm[i] = p->comm[i];
    r->comm[i] = 0;
    if (exit_count < PROC_EXIT_HISTORY) exit_count++;
}

/* 取第 nth 新的退出记录；pid/ppid 传 0 表示不按该字段过滤。 */
static struct proc_exit_record *proc_exit_nth(int pid, int ppid, int nth)
{
    u32 limit = 0xFFFFFFFFu;
    struct proc_exit_record *best = 0;
    int n, i;

    if (nth <= 0 || nth > exit_count) return 0;
    for (n = 0; n < nth; n++) {
        u32 best_seq = 0;
        best = 0;
        for (i = 0; i < PROC_EXIT_HISTORY; i++) {
            struct proc_exit_record *r = &exit_ring[i];
            if (!r->seq || r->seq >= limit) continue;
            if (pid > 0 && r->pid != pid) continue;
            if (ppid > 0 && r->ppid != ppid) continue;
            if (!best || r->seq > best_seq) {
                best = r;
                best_seq = r->seq;
            }
        }
        if (!best) return 0;
        limit = best->seq;
    }
    return best;
}

static void proc_zero_orphan(struct proc_orphan_event *e)
{
    int i;
    e->valid = 0;
    e->seq = 0;
    e->from = 0;
    e->to = 0;
    e->child_count = 0;
    for (i = 0; i < PROC_ORPHAN_CHILDREN; i++) e->children[i] = 0;
}

void process_boot(void)
{
    int i;
    for (i = 0; i < PROC_MAX; i++) {
        proc_zero(&procs[i]);
        procs[i].pid = -1;
    }
    proc_zero_orphan(&last_orphan);
    orphan_seq = 0;
    wait_block_total = 0;
    wait_wake_total = 0;
    join_block_total = 0;
    join_wake_total = 0;
    kernel_stack_reset();
}


static struct process *proc_find_active(int pid)
{
    int i;
    if (pid <= 0) return 0;
    for (i = 0; i < PROC_MAX; i++) {
        if ((procs[i].state == PROC_LIVE || procs[i].state == PROC_ZOMBIE) &&
            procs[i].pid == pid)
            return &procs[i];
    }
    return 0;
}

/* pending 只记录 1..31 号信号；推荐动作由投递方决定，不在这里改状态。 */
static void proc_signal_pending(struct process *p, int sig)
{
    if (!p || sig <= 0 || sig > 31) return;
    p->pending |= 1u << (sig - 1);
    p->last_sig = (u32)sig;
    p->sig_count++;
    if (sig == PROC_SIGCHLD) p->sigchld_count++;
}

/*
 * #DE 保存的 EIP 指向 faulting div/idiv 本身。这里只解码 P3.2 用到的
 * F6/F7 group：opcode + ModRM + 可选 SIB/位移，返回指令长度。
 * 其他异常返回 0，由原来的默认终止路径处理。
 */
static u32 proc_fault_skip_len(struct process *p, u32 vector, u32 eip)
{
    u8 buf[8];
    u32 i = 0;
    u32 op;
    u32 modrm;
    u32 mod;
    u32 rm;

    if (!p || !p->space || vector != EXC_DE) return 0;
    if (address_space_copyin(p->space, buf, eip, sizeof(buf)) != 0) return 0;

    if (buf[0] == 0x66) i++;
    if (i >= sizeof(buf)) return 0;
    op = buf[i++];
    if (op != 0xF6 && op != 0xF7) return 0;
    if (i >= sizeof(buf)) return 0;

    modrm = buf[i++];
    mod = modrm >> 6;
    rm = modrm & 7u;
    if (mod != 3 && rm == 4) {
        u32 sib;
        if (i >= sizeof(buf)) return 0;
        sib = buf[i++];
        if (mod == 0 && (sib & 7u) == 5) i += 4;
    }
    if (mod == 0 && rm == 5) i += 4;
    else if (mod == 1) i += 1;
    else if (mod == 2) i += 4;

    return i <= sizeof(buf) ? i : 0;
}

static u32 proc_sigreturn_trampoline_va(void)
{
    return AEVUM_USER_CODE_VA +
           (u32)((u32)user_sigreturn_trampoline - (u32)user_program_start);
}

int process_sigaction_current(int sig, u32 handler, u32 mask)
{
    struct process *self;

    if (sig <= 0 || sig >= PROC_NSIG) return -1;
    if (!task_current() || !task_process_current()) return -1;
    self = task_process_current();
    if (self->state != PROC_LIVE || !self->space) return -1;
    if (handler != PROC_SIG_DFL && handler != PROC_SIG_IGN &&
        !address_space_user_range_ok(self->space, handler, 1, 0))
        return -1;

    self->sigactions[sig].handler = handler;
    self->sigactions[sig].mask = mask & 0x7FFFFFFFu;
    self->sigactions[sig].flags = 0;
    return 0;
}

int process_signal_deliver(struct irq_frame *frame, int sig, u32 vector)
{
    struct process *self;
    struct proc_sigaction *action;
    struct proc_signal_frame sf;
    u32 frame_addr;
    u32 skip;
    u32 tramp;
    u32 frame_size;

    if (!frame || sig <= 0 || sig >= PROC_NSIG) return 0;
    if (!task_current() || !task_process_current()) return 0;
    self = task_process_current();
    if (self->state != PROC_LIVE || !self->space) return 0;
    if ((frame->cs & 3u) != 3u) return 0;

    action = &self->sigactions[sig];
    if (action->handler == PROC_SIG_DFL) return 0;

    skip = proc_fault_skip_len(self, vector, frame->eip);
    if (vector == EXC_DE && skip == 0) return 0;

    if (action->handler == PROC_SIG_IGN) {
        /* #DE 可安全前移；#BP 的保存 EIP 已经指向下一条。其他同步异常重试会死循环。 */
        if (skip == 0 && vector != EXC_BP) return 0;
        frame->eip += skip;
        self->sig_handled++;
        return 1;
    }

    if (!address_space_user_range_ok(self->space, action->handler, 1, 0))
        return 0;
    if (frame->user_esp < sizeof(sf) + 4u) return 0;

    frame_size = (u32)sizeof(sf) + 4u;
    frame_addr = (frame->user_esp - frame_size) & ~3u;
    if (frame_addr < AEVUM_USER_STACK_VA ||
        frame_addr + frame_size > AEVUM_USER_STACK_TOP)
        return 0;
    if (!address_space_user_range_ok(self->space, frame_addr, frame_size, 1))
        return 0;

    tramp = proc_sigreturn_trampoline_va();
    if (!address_space_user_range_ok(self->space, tramp, 1, 0)) return 0;

    sf.eax = frame->eax;
    sf.ebx = frame->ebx;
    sf.ecx = frame->ecx;
    sf.edx = frame->edx;
    sf.esi = frame->esi;
    sf.edi = frame->edi;
    sf.ebp = frame->ebp;
    sf.eip = frame->eip + skip;
    sf.cs = frame->cs;
    sf.eflags = frame->eflags;
    sf.user_esp = frame->user_esp;
    sf.ss = frame->ss;
    sf.sig = (u32)sig;
    sf.old_mask = self->sig_mask;
    sf.magic = PROC_SIGFRAME_MAGIC;

    if (address_space_copyout(self->space, frame_addr, &tramp, 4) != 0)
        return 0;
    if (address_space_copyout(self->space, frame_addr + 4, &sf,
                              (u32)sizeof(sf)) != 0)
        return 0;

    self->sig_mask = action->mask;
    self->sig_handled++;

    frame->eax = (u32)sig;
    frame->ebx = frame_addr + 4;
    frame->eip = action->handler;
    frame->user_esp = frame_addr;
    return 1;
}

int process_sigreturn_current(struct irq_frame *frame)
{
    struct process *self;
    struct proc_signal_frame sf;

    if (!frame) return -1;
    if (!task_current() || !task_process_current()) return -1;
    self = task_process_current();
    if (self->state != PROC_LIVE || !self->space) return -1;
    if (!address_space_user_range_ok(self->space, frame->user_esp,
                                     (u32)sizeof(sf), 0))
        return -1;
    if (address_space_copyin(self->space, &sf, frame->user_esp,
                             (u32)sizeof(sf)) != 0)
        return -1;
    if (sf.magic != PROC_SIGFRAME_MAGIC) return -1;
    if ((sf.cs & 3u) != 3u || (sf.ss & 3u) != 3u) return -1;
    if (!address_space_user_range_ok(self->space, sf.eip, 1, 0)) return -1;
    if (sf.user_esp < AEVUM_USER_STACK_VA ||
        sf.user_esp > AEVUM_USER_STACK_TOP)
        return -1;

    frame->eax = sf.eax;
    frame->ebx = sf.ebx;
    frame->ecx = sf.ecx;
    frame->edx = sf.edx;
    frame->esi = sf.esi;
    frame->edi = sf.edi;
    frame->ebp = sf.ebp;
    frame->eip = sf.eip;
    frame->cs = sf.cs;
    /* signal frame 来自用户栈，EFLAGS 必须按白名单过滤后才能恢复。 */
    frame->eflags = (sf.eflags & PROC_EFLAGS_USER_MASK) | PROC_EFLAGS_USER_FIXED;
    frame->user_esp = sf.user_esp;
    frame->ss = sf.ss;
    frame->vector = 0;
    frame->err = 0;
    self->sig_mask = sf.old_mask;
    return 0;
}

u32 process_sig_handled_current(void)
{
    struct process *self;

    if (!task_current() || !task_process_current()) return 0;
    self = task_process_current();
    return self->sig_handled;
}

static void proc_notify_parent(struct process *self)
{
    struct process *parent;

    if (!self || self->ppid <= 0) return;
    parent = proc_find_active(self->ppid);
    if (parent && parent->state == PROC_LIVE)
        proc_signal_pending(parent, PROC_SIGCHLD);
}

/*
 * P7.5：只在任务真的进入 TASK_ZOMBIE 后调用。此时目标已经不能再运行，
 * join 等待者重查一定能看到 ZOMBIE；若整个进程也转 ZOMBIE，再唤醒父进程。
 */
void process_notify_task_zombie(struct process *proc)
{
    struct process *parent;

    if (!proc) return;
    task_wake_process(proc);
    if (proc->state != PROC_ZOMBIE || proc->ppid <= 0) return;
    parent = proc_find_active(proc->ppid);
    if (parent && parent->state == PROC_LIVE)
        task_wake_process(parent);
}

static void proc_orphan_children(struct process *self)
{
    struct proc_orphan_event event;
    int i;

    if (!self) return;
    proc_zero_orphan(&event);
    event.from = self->pid;
    event.to = 0;
    for (i = 0; i < PROC_MAX; i++) {
        if ((procs[i].state == PROC_LIVE || procs[i].state == PROC_ZOMBIE) &&
            procs[i].ppid == self->pid) {
            procs[i].ppid = 0;
            if (event.child_count < PROC_ORPHAN_CHILDREN)
                event.children[event.child_count] = procs[i].pid;
            event.child_count++;
        }
    }
    if (event.child_count) {
        event.valid = 1;
        event.seq = ++orphan_seq;
        last_orphan = event;
    }
}

int process_last_orphan(struct proc_orphan_event *out)
{
    if (!out) return 0;
    *out = last_orphan;
    return out->valid;
}

void process_block_stats(u32 *wait_blocks, u32 *wait_wakes,
                         u32 *join_blocks, u32 *join_wakes)
{
    if (wait_blocks) *wait_blocks = wait_block_total;
    if (wait_wakes) *wait_wakes = wait_wake_total;
    if (join_blocks) *join_blocks = join_block_total;
    if (join_wakes) *join_wakes = join_wake_total;
}

/* 连续 fork/wait 时优先复用刚退出的直接子 PID，摘要来自 exit ring。 */
static int proc_reusable_child_pid(const struct process *parent)
{
    struct proc_exit_record *r;

    if (!parent) return 0;
    r = proc_exit_nth(0, parent->pid, 1);
    return r ? r->pid : 0;
}

static struct process *proc_alloc_slot(int pid)
{
    struct process *slot = 0;
    int i;

    /* 优先空槽；P10 后回收直接清零，DEAD 分支仅保留兼容。 */
    for (i = 0; i < PROC_MAX; i++) {
        if (procs[i].state == PROC_FREE) {
            slot = &procs[i];
            break;
        }
    }
    if (!slot) {
        for (i = 0; i < PROC_MAX; i++) {
            if (procs[i].state == PROC_DEAD) {
                slot = &procs[i];
                break;
            }
        }
    }
    if (!slot) return 0;

    /* PID 可能被复用；清掉其他同 PID 的旧槽，避免 V 页出现重复行。 */
    for (i = 0; i < PROC_MAX; i++) {
        if (&procs[i] != slot && procs[i].state == PROC_DEAD &&
            procs[i].pid == pid)
            proc_zero(&procs[i]);
    }
    proc_zero(slot);
    slot->pid = pid;
    return slot;
}

/* 回收一个 ZOMBIE：线程任务槽、线程内核栈、地址空间、TGID 一次释放。 */
static void process_reap(struct process *p)
{
    if (!p || p->state != PROC_ZOMBIE) return;
    if (task_process_current() == p) return;
    /*
     * 线程组里仍有非 ZOMBIE 任务在跑时不能回收：此时释放地址空间
     * 等于拆正在跑的台。保留 ZOMBIE，等下一轮 reaper 再试。
     */
    if (task_process_live_threads(p) != 0) return;
    if (task_release_process_threads(p) != 0) return;
    proc_fd_table_close_all(&p->fds);
    if (p->space) address_space_release(p->space);
    task_pid_release(p->pid);
    /*
     * P10：先把 pid/ppid/exit_code/term_sig/镜像和 comm 复制到 exit ring，
     * 再清空 PCB。之后这个槽不再保留任何可被 wait 或 V 页误读的墓碑。
     */
    proc_exit_push(p);
    proc_zero(p);
}

static int proc_copy_name(struct process *p, const char *name)
{
    int i;
    if (!name) return -1;
    for (i = 0; i < PROC_NAME_MAX && name[i]; i++) p->comm[i] = name[i];
    p->comm[i] = 0;
    return 0;
}

struct process *process_create_user(int priority, const char *name,
                                    const u8 *code, u32 code_size,
                                    u32 user_stack_top)
{
    struct process *p;
    struct task_struct *task;
    struct address_space *space;
    int pid;
    u32 stack_top;
    const char *rc;

    if (!name || !code || !code_size) return 0;

    pid = task_pid_reserve();
    if (!pid) return 0;
    p = proc_alloc_slot(pid);
    if (!p) {
        task_pid_release(pid);
        return 0;
    }
    stack_top = kernel_stack_alloc();
    if (!stack_top) {
        proc_zero(p);
        task_pid_release(pid);
        return 0;
    }
    space = address_space_create_user(code, code_size);
    if (!space) {
        kernel_stack_free(stack_top);
        proc_zero(p);
        task_pid_release(pid);
        return 0;
    }

    p->state = PROC_LIVE;
    p->ppid = (task_current() && task_process_current())
                  ? task_process_current()->pid
                  : 0;
    p->stack_top = stack_top;
    p->space = space;
    p->exit_code = 0;
    proc_copy_name(p, name);
    proc_fd_console_open(&p->fds);

    rc = task_create_user_with_pid(priority, name, AEVUM_USER_CODE_VA,
                                   stack_top, space, user_stack_top,
                                   p, pid, &task);
    if (rc) {
        address_space_release(space);
        kernel_stack_free(stack_top);
        proc_zero(p);
        task_pid_release(pid);
        return 0;
    }
    return p;
}

struct process *process_fork(struct irq_frame *frame)
{
    struct process *parent;
    struct process *p;
    struct task_struct *task;
    struct address_space *space;
    int pid;
    int i;
    u32 stack_top;
    const char *rc;

    if (!frame) return 0;
    parent = (task_current() && task_process_current())
                 ? task_process_current()
                 : 0;
    if (!parent || parent->state != PROC_LIVE) return 0;
    if ((frame->cs & 3u) != 3u) return 0;

    pid = proc_reusable_child_pid(parent);
    if (pid && task_pid_reserve_hint(pid) != pid) pid = 0;
    if (!pid) pid = task_pid_reserve();
    if (!pid) return 0;
    p = proc_alloc_slot(pid);
    if (!p) {
        task_pid_release(pid);
        return 0;
    }
    stack_top = kernel_stack_alloc();
    if (!stack_top) {
        proc_zero(p);
        task_pid_release(pid);
        return 0;
    }
    space = address_space_clone(parent->space);
    if (!space) {
        kernel_stack_free(stack_top);
        proc_zero(p);
        task_pid_release(pid);
        return 0;
    }

    p->state = PROC_LIVE;
    p->ppid = parent->pid;
    p->stack_top = stack_top;
    p->space = space;
    p->exit_code = 0;
    proc_copy_name(p, "userE");
    /* 信号动作与 mask 随 fork 继承；pending 不继承。 */
    for (i = 0; i < PROC_NSIG; i++) p->sigactions[i] = parent->sigactions[i];
    p->sig_mask = parent->sig_mask;
    p->image_id = parent->image_id;
    p->exec_argc = parent->exec_argc;
    p->exec_envc = parent->exec_envc;
    if (proc_fd_table_copy(&p->fds, &parent->fds) != 0) {
        address_space_release(space);
        kernel_stack_free(stack_top);
        proc_zero(p);
        task_pid_release(pid);
        return 0;
    }

    rc = task_fork_user(pid, "userE", frame, stack_top, space, p, &task);
    if (rc) {
        address_space_release(space);
        kernel_stack_free(stack_top);
        proc_zero(p);
        task_pid_release(pid);
        return 0;
    }
    return p;
}

static const u8 *proc_exec_image(int image_id, u32 *size)
{
    if (!size) return 0;
    if (image_id == 1) {
        *size = (u32)((u32)user_exec_end - (u32)user_exec_start);
        return user_exec_start;
    }
    if (image_id == 2) {
        *size = (u32)((u32)user_pipe_exec_end - (u32)user_pipe_exec_start);
        return user_pipe_exec_start;
    }
    return 0;
}

/* 从旧 AS 读一个 NUL 结尾的短字符串；超过 31 字节或跨非法 VMA 都失败。 */
static int proc_exec_read_string(const struct address_space *space, u32 va,
                                 char *out)
{
    u32 i;

    if (!va || va > 0xFFFFFFE0u) return -1;
    for (i = 0; i < PROC_EXEC_STRING_MAX; i++) {
        if (address_space_copyin(space, &out[i], va + i, 1) != 0)
            return -1;
        if (!out[i]) return 0;
    }
    return -1;
}

/* 读 argv/envp 数组；返回条目数，数组必须有 NULL 结尾且不超过 4 项。 */
static int proc_exec_read_vec(const struct address_space *space, u32 vec,
                              char *out)
{
    u32 i;
    u32 p;

    if (!vec) return 0;
    if (vec > 0xFFFFFFE0u) return -1;
    for (i = 0; i <= PROC_EXEC_MAX_VEC; i++) {
        if (address_space_copyin(space, &p, vec + i * 4u, 4) != 0)
            return -1;
        if (!p) return (int)i;
        if (i == PROC_EXEC_MAX_VEC) return -1;
        if (proc_exec_read_string(space, p, out + i * PROC_EXEC_STRING_MAX) != 0)
            return -1;
    }
    return -1;
}

static int proc_exec_parse(const struct address_space *space, u32 argv, u32 envp,
                           struct proc_exec_args *args)
{
    int n;

    args->argc = 0;
    args->envc = 0;
    n = proc_exec_read_vec(space, argv, (char *)args->argv);
    if (n < 0) return -1;
    args->argc = (u32)n;
    n = proc_exec_read_vec(space, envp, (char *)args->envp);
    if (n < 0) return -1;
    args->envc = (u32)n;
    return 0;
}

static u32 proc_exec_strlen(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

/*
 * 把参数复制到新 AS 的栈页，布局从低到高：
 * argc, argv[0..argc-1], NULL, envp[0..envc-1], NULL, 字符串区。
 * 返回新的用户 esp；栈页容不下时失败，调用方负责释放新 AS。
 */
static int proc_exec_build_stack(struct address_space *space,
                                 const struct proc_exec_args *args,
                                 u32 *esp_out)
{
    u32 argv_words = (args->argc + 1u) * 4u;
    u32 envp_words = (args->envc + 1u) * 4u;
    u32 string_bytes = 0;
    u32 strings_base, argv_addr, envp_addr, esp, str_va;
    u32 i, len;
    u32 zero = 0;

    for (i = 0; i < args->argc; i++)
        string_bytes += proc_exec_strlen(args->argv[i]) + 1u;
    for (i = 0; i < args->envc; i++)
        string_bytes += proc_exec_strlen(args->envp[i]) + 1u;
    if (4u + argv_words + envp_words + string_bytes > AEVUM_PAGE_SIZE)
        return -1;

    strings_base = (AEVUM_USER_STACK_TOP - string_bytes) & ~3u;
    envp_addr = strings_base - envp_words;
    argv_addr = envp_addr - argv_words;
    esp = argv_addr - 4u;
    if (esp < AEVUM_USER_STACK_VA) return -1;

    if (address_space_copyout(space, esp, &args->argc, 4) != 0)
        return -1;

    str_va = strings_base;
    for (i = 0; i < args->argc; i++) {
        len = proc_exec_strlen(args->argv[i]) + 1u;
        if (address_space_copyout(space, str_va, args->argv[i], len) != 0)
            return -1;
        if (address_space_copyout(space, argv_addr + i * 4u, &str_va, 4) != 0)
            return -1;
        str_va += len;
    }
    if (address_space_copyout(space, argv_addr + args->argc * 4u, &zero, 4) != 0)
        return -1;

    for (i = 0; i < args->envc; i++) {
        len = proc_exec_strlen(args->envp[i]) + 1u;
        if (address_space_copyout(space, str_va, args->envp[i], len) != 0)
            return -1;
        if (address_space_copyout(space, envp_addr + i * 4u, &str_va, 4) != 0)
            return -1;
        str_va += len;
    }
    if (address_space_copyout(space, envp_addr + args->envc * 4u, &zero, 4) != 0)
        return -1;

    *esp_out = esp;
    return 0;
}

/*
 * P7.0：直接创建新进程，不复制父地址空间，也不要求父先 fork。
 * 参数仍从父 AS 解析；子进程拿到新的 AS、内核栈、PCB 和独立用户栈。
 * 这是 fork+exec 之外的另一条创建路径，后续设备/资源句柄也从这里接入。
 */
struct process *process_spawn_current(int priority, const char *name,
                                      int image_id, u32 argv, u32 envp)
{
    struct process *parent;
    struct process *p;
    struct task_struct *task;
    struct address_space *space;
    struct proc_exec_args args;
    const u8 *code;
    u32 code_size;
    u32 new_esp;
    u32 stack_top;
    int pid;
    const char *rc;

    if (!task_current() || !task_process_current()) return 0;
    parent = task_process_current();
    if (parent->state != PROC_LIVE || !parent->space) return 0;
    if (!name) name = "spawn";

    code = proc_exec_image(image_id, &code_size);
    if (!code || !code_size) return 0;
    if (proc_exec_parse(parent->space, argv, envp, &args) != 0) return 0;

    pid = task_pid_reserve();
    if (!pid) return 0;
    p = proc_alloc_slot(pid);
    if (!p) {
        task_pid_release(pid);
        return 0;
    }
    stack_top = kernel_stack_alloc();
    if (!stack_top) {
        proc_zero(p);
        task_pid_release(pid);
        return 0;
    }
    space = address_space_create_user(code, code_size);
    if (!space) {
        kernel_stack_free(stack_top);
        proc_zero(p);
        task_pid_release(pid);
        return 0;
    }
    if (proc_exec_build_stack(space, &args, &new_esp) != 0) {
        address_space_release(space);
        kernel_stack_free(stack_top);
        proc_zero(p);
        task_pid_release(pid);
        return 0;
    }

    p->state = PROC_LIVE;
    p->ppid = parent->pid;
    p->stack_top = stack_top;
    p->space = space;
    p->exit_code = 0;
    p->image_id = (u32)image_id;
    p->exec_argc = args.argc;
    p->exec_envc = args.envc;
    proc_copy_name(p, name);

    /* spawn 继承父进程的句柄表；fd 引用由 fd 模块负责加减。 */
    if (proc_fd_table_copy(&p->fds, &parent->fds) != 0) {
        address_space_release(space);
        kernel_stack_free(stack_top);
        proc_zero(p);
        task_pid_release(pid);
        return 0;
    }

    rc = task_create_user_with_pid(priority, name, AEVUM_USER_CODE_VA,
                                   stack_top, space, new_esp, p, pid, &task);
    if (rc) {
        proc_fd_table_close_all(&p->fds);
        address_space_release(space);
        kernel_stack_free(stack_top);
        proc_zero(p);
        task_pid_release(pid);
        return 0;
    }
    return p;
}

/*
 * P7.1：clone 的受控线程子集。线程不分配 process 槽，不复制 AS/fd；
 * 只申请 TID 和内核栈，然后和当前进程共用地址空间。
 */
int process_clone_thread_current(int priority, const char *name,
                                 u32 entry, u32 user_stack, u32 flags, u32 arg)
{
    struct process *self;
    struct task_struct *task;
    u32 stack_top;
    int tid;
    const char *rc;

    if (!task_current() || !task_process_current()) return -1;
    self = task_process_current();
    (void)priority;
    if (self->state != PROC_LIVE || !self->space) return -1;
    if ((flags & ~PROC_CLONE_THREAD_FLAGS) != 0) return -1;
    if ((flags & PROC_CLONE_THREAD_FLAGS) != PROC_CLONE_THREAD_FLAGS)
        return -1;
    if (!entry || user_stack < 4u) return -1;
    if (!address_space_user_range_ok(self->space, entry, 1, 0)) return -1;
    if (!address_space_user_range_ok(self->space, user_stack - 4u, 4, 1))
        return -1;

    tid = task_pid_reserve();
    if (!tid) return -1;
    stack_top = kernel_stack_alloc();
    if (!stack_top) {
        task_pid_release(tid);
        return -1;
    }

    if (!name) name = "userT";
    rc = task_create_thread_user(tid, name, self, self->space, entry, stack_top,
                                 user_stack, arg, &task);
    if (rc) {
        kernel_stack_free(stack_top);
        task_pid_release(tid);
        return -1;
    }
    return tid;
}

/*
 * P4.2 exec：PID/PPID/任务槽保持不变，旧 AS 中的 argv/envp 先复制到新栈，
 * 再切 CR3、释放旧 AS。解析、AS 分配或栈构造失败时旧 AS/frame 都不动。
 */
int process_exec_current(struct irq_frame *frame, int image_id, u32 argv, u32 envp)
{
    struct process *self;
    struct address_space *old_space;
    struct address_space *new_space;
    struct proc_exec_args args;
    const u8 *code;
    u32 code_size;
    u32 new_esp;
    u32 flags;

    if (!frame || (frame->cs & 3u) != 3u) return -1;
    if (!task_current() || !task_process_current()) return -1;
    self = task_process_current();
    if (self->state != PROC_LIVE || !self->space) return -1;
    /* P7.1 尚未实现多线程 exec：先拒绝，避免其他线程继续指向旧 AS。 */
    if (task_process_live_threads(self) > 1) return -1;

    code = proc_exec_image(image_id, &code_size);
    if (!code || !code_size) return -1;
    if (proc_exec_parse(self->space, argv, envp, &args) != 0) return -1;

    new_space = address_space_create_user(code, code_size);
    if (!new_space) return -1;
    if (proc_exec_build_stack(new_space, &args, &new_esp) != 0) {
        address_space_release(new_space);
        return -1;
    }

    old_space = self->space;
    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    address_space_switch(new_space);
    task_set_space(task_current(), new_space);
    self->space = new_space;
    self->image_id = image_id;
    self->exec_argc = args.argc;
    self->exec_envc = args.envc;
    proc_reset_signals(self);
    self->exit_code = 0;

    frame->eax = 0;
    frame->ebx = 0;
    frame->ecx = 0;
    frame->edx = 0;
    frame->esi = 0;
    frame->edi = 0;
    frame->ebp = 0;
    frame->eip = AEVUM_USER_CODE_VA;
    frame->cs = 0x1B;
    frame->eflags = 0x202;
    frame->user_esp = new_esp;
    frame->ss = 0x23;

    address_space_release(old_space);
    __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
    return 0;
}

int process_exit_current(u32 code)
{
    struct process *self;

    if (!task_current() || !task_process_current()) return -1;
    self = task_process_current();
    if (self->state != PROC_LIVE) return -1;
    task_terminate_threads(self, task_current_pid());
    proc_fd_table_close_all(&self->fds);
    self->state = PROC_ZOMBIE;
    self->term_sig = 0;
    self->exit_code = code & 0xFFu;

    /* 父进程先退：直接子进程变成 0 号孤儿，交给调度器 reaper。 */
    proc_orphan_children(self);
    proc_notify_parent(self);
    return 0;
}

int process_thread_exit_current(u32 code)
{
    struct process *self;

    if (!task_current() || !task_process_current()) return -1;
    self = task_process_current();
    if (self->state != PROC_LIVE) return -1;

    task_set_current_exit_code(code);
    if (task_process_live_threads(self) <= 1)
        return process_exit_current(code);
    return 0;
}

int process_thread_join_current(int tid, u32 user_status)
{
    struct process *self;
    struct task_info info;
    struct proc_join_ctx block_ctx;
    int block_rc;

    if (!task_current() || !task_process_current()) return -1;
    self = task_process_current();
    if (!self->space) return -1;
    if (user_status &&
        !address_space_user_range_ok(self->space, user_status, 4, 1))
        return -1;
    if (tid <= 0 || tid == task_current_pid()) return -1;
    block_ctx.self = self;
    block_ctx.tid = tid;

    for (;;) {
        if (!task_thread_snapshot(self, tid, &info)) return -1;
        if (info.detached) return -1;
        if (info.state == TASK_ZOMBIE) break;
        block_rc = task_block_until(proc_join_ready, &block_ctx);
        if (block_rc < 0) return -1;
        if (block_rc > 0) {
            join_block_total++;
            join_wake_total++;
        }
    }

    if (user_status &&
        address_space_copyout(self->space, user_status, &info.exit_code, 4) != 0)
        return -1;
    if (task_reap_thread(self, tid) != 0) return -1;
    return 0;
}

int process_thread_detach_current(int tid)
{
    struct process *self;

    if (!task_current() || !task_process_current()) return -1;
    self = task_process_current();
    if (!self->space || self->state != PROC_LIVE) return -1;
    if (tid <= 0) return -1;
    return task_detach_thread(self, tid);
}

int process_gettid_current(void)
{
    return task_current_pid();
}

int process_terminate_current(int sig)
{
    struct process *self;

    if (sig <= 0 || sig > 31) return -1;
    if (!task_current() || !task_process_current()) return -1;
    self = task_process_current();
    if (self->state != PROC_LIVE) return -1;

    task_terminate_threads(self, task_current_pid());
    proc_fd_table_close_all(&self->fds);
    proc_signal_pending(self, sig);
    self->state = PROC_ZOMBIE;
    self->term_sig = (u32)sig;
    self->exit_code = PROC_WAIT_STATUS(sig);
    proc_orphan_children(self);
    proc_notify_parent(self);
    return 0;
}

static int proc_has_children(const struct process *self)
{
    int i;
    for (i = 0; i < PROC_MAX; i++)
        if ((procs[i].state == PROC_LIVE || procs[i].state == PROC_ZOMBIE) &&
            procs[i].ppid == self->pid)
            return 1;
    return 0;
}

static int proc_is_direct_child(const struct process *self, int pid);

static int proc_has_zombie_child(const struct process *self, int pid)
{
    int i;
    for (i = 0; i < PROC_MAX; i++)
        if (procs[i].state == PROC_ZOMBIE &&
            procs[i].ppid == self->pid &&
            (pid <= 0 || procs[i].pid == pid))
            return 1;
    return 0;
}

/*
 * 在关中断窗口里重查：只要出现可回收的 ZOMBIE、指定 PID 已消失、或已经没有
 * 子进程，就返回 1 让外层重新走完整扫描/错误路径；否则返回 0 进入 BLOCKED。
 */
static int proc_wait_ready(void *arg)
{
    struct proc_wait_ctx *ctx = (struct proc_wait_ctx *)arg;
    if (!ctx || !ctx->self) return 1;
    if (proc_has_zombie_child(ctx->self, ctx->pid)) return 1;
    if (ctx->pid > 0 && !proc_is_direct_child(ctx->self, ctx->pid)) return 1;
    if (!proc_has_children(ctx->self)) return 1;
    return 0;
}

static int proc_join_ready(void *arg)
{
    struct proc_join_ctx *ctx = (struct proc_join_ctx *)arg;
    struct task_info info;

    if (!ctx || !ctx->self) return 1;
    if (!task_thread_snapshot(ctx->self, ctx->tid, &info)) return 1;
    if (info.detached) return 1;
    if (info.state == TASK_ZOMBIE) return 1;
    return 0;
}

/* pid>0 只接受自己的直接子进程；ZOMBIE 也算，尚未被 wait 消费。 */
static int proc_is_direct_child(const struct process *self, int pid)
{
    int i;
    if (!self || pid <= 0) return 0;
    for (i = 0; i < PROC_MAX; i++)
        if ((procs[i].state == PROC_LIVE || procs[i].state == PROC_ZOMBIE) &&
            procs[i].ppid == self->pid && procs[i].pid == pid)
            return 1;
    return 0;
}

static int process_wait_common(int pid, u32 user_status, u32 ticks, int timed)
{
    struct process *self;
    struct proc_wait_ctx block_ctx;
    int i;
    int block_rc;

    if (!task_current() || !task_process_current()) return -1;
    self = task_process_current();
    if (!address_space_user_range_ok(self->space, user_status, 4, 1)) return -1;
    if (pid > 0 && !proc_is_direct_child(self, pid)) return -1;
    if (!proc_has_children(self)) return -1;
    block_ctx.self = self;
    block_ctx.pid = pid;

    for (;;) {
        for (i = 0; i < PROC_MAX; i++) {
            struct process *child = &procs[i];
            if (child->state != PROC_ZOMBIE) continue;
            if (child->ppid != self->pid) continue;
            if (pid > 0 && child->pid != pid) continue;

            if (address_space_copyout(self->space, user_status,
                                      &child->exit_code, 4) != 0)
                return -1;
            {
                int child_pid = child->pid;
                /*
                 * 退出摘要由 process_reap() 统一写入 exit ring；wait 只负责
                 * copyout status 和消费一次 SIGCHLD pending。
                 */
                process_reap(child);
                self->pending &= ~(1u << (PROC_SIGCHLD - 1));
                return child_pid;
            }
        }

        /* 指定 PID 已经不存在或所有子进程都没了：不要永久等。 */
        if (pid > 0 && !proc_is_direct_child(self, pid)) return -1;
        if (!proc_has_children(self)) return -1;
        if (timed) {
            block_rc = task_block_until_timeout(proc_wait_ready, &block_ctx, ticks);
            if (block_rc < 0) return -1;
            if (block_rc > 0) return 0;      /* P11.1：timeout */
        } else {
            block_rc = task_block_until(proc_wait_ready, &block_ctx);
            if (block_rc < 0) return -1;
            if (block_rc > 0) {
                wait_block_total++;
                wait_wake_total++;
            }
        }
    }
}

int process_wait(int pid, u32 user_status)
{
    return process_wait_common(pid, user_status, 0, 0);
}

int process_wait_timeout(int pid, u32 user_status, u32 ticks)
{
    return process_wait_common(pid, user_status, ticks, 1);
}

int process_getpid_current(void)
{
    return (task_current() && task_process_current())
               ? task_process_current()->pid
               : task_current_pid();
}

int process_getppid_current(void)
{
    return (task_current() && task_process_current())
               ? task_process_current()->ppid
               : 0;
}

int process_alive(int pid)
{
    struct process *p = proc_find_active(pid);
    if (!p || p->state != PROC_LIVE) return 0;
    return task_process_live_threads(p) > 0;
}

int process_live_count(void)
{
    int i, n = 0;
    for (i = 0; i < PROC_MAX; i++)
        if (procs[i].state == PROC_LIVE) n++;
    return n;
}

void process_stop_all(void)
{
    int i;
    for (i = 0; i < PROC_MAX; i++)
        if (procs[i].state == PROC_LIVE && procs[i].space)
            address_space_set_stop(procs[i].space, 1);
}

/* 新 V 演示开始前回收上一轮 ZOMBIE；P10 后 process_reap 会直接清零 PCB。 */
void process_cleanup(void)
{
    int i;
    for (i = 0; i < PROC_MAX; i++)
        if (procs[i].state == PROC_ZOMBIE)
            process_reap(&procs[i]);
    for (i = 0; i < PROC_MAX; i++)
        if (procs[i].state == PROC_DEAD)
            proc_zero(&procs[i]);
    last_orphan.valid = 0;
}

void process_reap_orphans(void)
{
    int i;
    int current_pid = task_process_current()
                          ? process_pid_of(task_process_current())
                          : task_current_pid();

    for (i = 0; i < PROC_MAX; i++) {
        struct process *p = &procs[i];
        struct process *parent;

        if (p->state != PROC_ZOMBIE || p->pid == current_pid) continue;
        parent = proc_find_active(p->ppid);
        if (p->ppid == 0 || !parent || parent->state != PROC_LIVE)
            process_reap(p);
    }
}

int process_snapshot(int slot, struct process_info *out)
{
    struct process *p;
    struct task_info info;
    int has_task;

    if (!out || slot < 0 || slot >= PROC_MAX) return 0;
    p = &procs[slot];
    if (p->state == PROC_FREE) return 0;

    out->used = 1;
    out->pid = p->pid;
    out->ppid = p->ppid;
    out->state = p->state;
    out->thread_count = task_process_thread_count(p);
    out->tid = 0;
    out->exit_code = p->exit_code;
    out->stack_top = p->stack_top;
    out->cr3 = address_space_cr3(p->space);
    out->asid = address_space_id(p->space);
    out->counter = address_space_counter(p->space);
    out->code_phys = address_space_code_phys(p->space);
    out->data_phys = address_space_data_phys(p->space);
    out->brk = address_space_brk(p->space);
    out->user_pages = address_space_user_pages(p->space);
    out->cow_pages = address_space_cow_pages(p->space);
    out->vma_count = address_space_vma_count(p->space);
    out->pending = p->pending;
    out->last_sig = p->last_sig;
    out->term_sig = p->term_sig;
    out->sig_count = p->sig_count;
    out->sigchld_count = p->sigchld_count;
    out->sig_handled = p->sig_handled;
    out->sig_mask = p->sig_mask;
    out->image_id = p->image_id;
    out->exec_argc = p->exec_argc;
    out->exec_envc = p->exec_envc;

    has_task = task_snapshot_process(p, &info);
    if (has_task) {
        int i;
        out->tid = info.pid;
        out->stack_top = info.stack_top;
        out->task_state = info.state;
        out->switches = info.switches;
        out->cpu_ticks = info.cpu_ticks;
        for (i = 0; i <= PROC_NAME_MAX; i++) out->comm[i] = info.comm[i];
    } else {
        int i;
        out->task_state = TASK_UNUSED;
        out->switches = 0;
        out->cpu_ticks = 0;
        for (i = 0; i <= PROC_NAME_MAX && p->comm[i]; i++)
            out->comm[i] = p->comm[i];
        out->comm[i] = 0;
    }
    return 1;
}

int process_count(void)
{
    return PROC_MAX;
}

int process_slot_used(void)
{
    int i, n = 0;

    for (i = 0; i < PROC_MAX; i++)
        if (procs[i].state == PROC_LIVE || procs[i].state == PROC_ZOMBIE)
            n++;
    return n;
}

int process_pid_of(struct process *proc)
{
    return proc ? proc->pid : 0;
}

struct address_space *process_space(struct process *proc)
{
    return proc ? proc->space : 0;
}

struct address_space *process_slot_space(int slot)
{
    if (slot < 0 || slot >= PROC_MAX) return 0;
    if (procs[slot].state == PROC_FREE) return 0;
    return procs[slot].space;
}

int process_slot_fd_info(int slot, int fd, struct proc_fd_info *out)
{
    if (slot < 0 || slot >= PROC_MAX) return 0;
    if (procs[slot].state == PROC_FREE) return 0;
    return proc_fd_snapshot(&procs[slot].fds, fd, out);
}

struct proc_fd_table *process_fds(struct process *proc)
{
    return proc ? &proc->fds : 0;
}

int process_reaped_exec(int parent_pid, int slot, int *pid,
                        u32 *image_id, u32 *argc, u32 *envc)
{
    struct process *p = proc_find_active(parent_pid);
    struct proc_exit_record *r;

    if (!p || p->state != PROC_LIVE || slot < 0 || slot >= PROC_REAP_SLOTS)
        return -1;
    r = proc_exit_nth(0, parent_pid, slot == 0 ? 2 : 1);
    if (!r) return -1;
    if (pid) *pid = r->pid;
    if (image_id) *image_id = r->image_id;
    if (argc) *argc = r->argc;
    if (envc) *envc = r->envc;
    return 0;
}

int process_exit_snapshot(int index, struct process_exit_info *out)
{
    struct proc_exit_record *r;
    int i;

    if (!out || index < 0) return -1;
    r = proc_exit_nth(0, 0, index + 1);
    if (!r) return -1;
    out->valid = 1;
    out->seq = r->seq;
    out->pid = r->pid;
    out->ppid = r->ppid;
    out->exit_code = r->exit_code;
    out->term_sig = r->term_sig;
    out->image_id = r->image_id;
    out->argc = r->argc;
    out->envc = r->envc;
    for (i = 0; i <= PROC_NAME_MAX; i++) out->comm[i] = r->comm[i];
    return 0;
}

u32 process_read32(int pid, u32 va)
{
    struct process *p = proc_find_active(pid);
    if (!p || !p->space) return 0;
    return address_space_user_read32(p->space, va);
}

int process_image_id(int pid)
{
    struct proc_exit_record *r;
    int i;
    int image = 0;

    if (pid <= 0) return 0;
    for (i = 0; i < PROC_MAX; i++) {
        if (procs[i].state != PROC_FREE && procs[i].pid == pid &&
            (int)procs[i].image_id > image)
            image = (int)procs[i].image_id;
    }
    r = proc_exit_nth(pid, 0, 1);
    if (r && (int)r->image_id > image) image = (int)r->image_id;
    return image;
}

int process_exec_counts(int pid, u32 *argc, u32 *envc)
{
    struct proc_exit_record *r;
    int i;
    int best = -1;
    u32 best_image = 0;

    if (!argc || !envc || pid <= 0) return -1;
    *argc = 0;
    *envc = 0;
    for (i = 0; i < PROC_MAX; i++) {
        if (procs[i].state == PROC_FREE || procs[i].pid != pid) continue;
        if (best < 0 || procs[i].image_id > best_image) {
            best = i;
            best_image = procs[i].image_id;
        }
    }
    if (best >= 0) {
        *argc = procs[best].exec_argc;
        *envc = procs[best].exec_envc;
        return 0;
    }
    r = proc_exit_nth(pid, 0, 1);
    if (!r) return -1;
    *argc = r->argc;
    *envc = r->envc;
    return 0;
}
