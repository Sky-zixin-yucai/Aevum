#ifndef AEVUM_KERNEL_PROC_H
#define AEVUM_KERNEL_PROC_H

#include "kernel/types.h"

struct address_space;
struct task_struct;
struct irq_frame;
struct proc_fd_table;
struct proc_fd_info;

#define PROC_MAX 12
#define PROC_NAME_MAX 15

/*
 * P10：PCB 被 wait/reaper 真正清零前，退出摘要进入全局 ring。
 * ring 只保存显示和 wait 历史需要的字段，不保留 AS/fd/任务指针。
 */
#define PROC_EXIT_HISTORY 4

/* 最近一次“父进程退出，直接子进程改挂 PID 0”的只读快照。 */
#define PROC_ORPHAN_CHILDREN 4

struct process_exit_info {
    int valid;
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

struct proc_orphan_event {
    int valid;
    u32 seq;
    int from;
    int to;
    int child_count;
    int children[PROC_ORPHAN_CHILDREN];
};

/* P3.2：同步异常先查每进程 handler；无 handler 才走 P3.1 的默认终止。 */
#define PROC_NSIG    32
#define PROC_SIG_DFL 0u
#define PROC_SIG_IGN 1u
#define PROC_SIGILL  4
#define PROC_SIGTRAP 5
#define PROC_SIGFPE  8
#define PROC_SIGSEGV 11
#define PROC_SIGCHLD 17

/*
 * P7.1 只接受线程组所需的 clone 子集。FS 目前没有 cwd/root 状态，
 * 不声称支持 CLONE_FS；其余 flag 一律拒绝。
 */
#define PROC_CLONE_VM       0x00000100u
#define PROC_CLONE_FILES    0x00000400u
#define PROC_CLONE_SIGHAND  0x00000800u
#define PROC_CLONE_THREAD   0x00010000u
#define PROC_CLONE_THREAD_FLAGS \
    (PROC_CLONE_VM | PROC_CLONE_FILES | PROC_CLONE_SIGHAND | PROC_CLONE_THREAD)

/* wait status 高位置 1 表示被信号终止；正常退出仍保留原 exit code。 */
#define PROC_WAIT_SIGNAL_FLAG 0x80000000u
#define PROC_WAIT_STATUS(sig) (PROC_WAIT_SIGNAL_FLAG | ((u32)(sig) & 0x7Fu))
#define PROC_WAIT_IS_SIGNAL(status) (((status) & PROC_WAIT_SIGNAL_FLAG) != 0)
#define PROC_WAIT_SIGNAL(status) ((status) & 0x7Fu)
#define PROC_WAIT_EXIT_CODE(status) ((status) & 0xFFu)

/*
 * 用户进程生命周期。PID、父子关系、退出码、信号和 wait 状态归本模块；
 * 调度、内核栈和 FPU 现场仍归 task 模块；页表和用户页归 address_space。
 */
enum proc_state {
    PROC_FREE = 0,
    PROC_LIVE = 1,
    PROC_ZOMBIE = 2,
    PROC_DEAD = 3
};

struct process_info {
    int used;
    int pid;
    int ppid;
    int tid;            /* 首个可观测线程的 TID */
    int thread_count;
    int state;
    int task_state;
    u32 exit_code;
    char comm[PROC_NAME_MAX + 1];
    u32 stack_top;
    u32 cr3;
    u32 asid;
    u32 counter;
    u32 code_phys;
    u32 data_phys;
    u32 brk;
    u32 user_pages;
    u32 cow_pages;
    u32 vma_count;
    u32 switches;
    u32 cpu_ticks;
    u32 pending;
    u32 last_sig;
    u32 term_sig;
    u32 sig_count;
    u32 sigchld_count;
    u32 sig_handled;
    u32 sig_mask;
    u32 image_id;
    u32 exec_argc;
    u32 exec_envc;
};

void process_boot(void);

/* 由 kernel task 创建用户进程：保留 PID，动态分配栈池槽与 AS，最后才发布任务 READY。 */
struct process *process_create_user(int priority, const char *name,
                                    const u8 *code, u32 code_size,
                                    u32 user_stack_top);

/* fork 当前 ring3 进程：复制 VMA 用户页、内核帧和 FPU；父返回子 PID，子返回 0。 */
struct process *process_fork(struct irq_frame *frame);

/*
 * P7.0：从当前 ring3 进程直接创建一个新进程，不复制父地址空间。
 * argv/envp 仍指向父 AS 的用户指针；内核先解析参数，再为子进程建独立 AS、
 * 内嵌镜像栈和 PCB。成功返回子 process，失败返回 0；子进程 eax=0。
 */
struct process *process_spawn_current(int priority, const char *name,
                                      int image_id, u32 argv, u32 envp);

/*
 * P7.1 线程组最小闭环：clone 只接受 PROC_CLONE_THREAD_FLAGS，子任务共享
 * 当前 AS/fd/信号动作；成功返回 TID，失败返回 -1。entry 从 eax 收到 arg。
 */
int process_clone_thread_current(int priority, const char *name,
                                 u32 entry, u32 user_stack, u32 flags, u32 arg);

/* 线程退出：只标当前任务；最后一个活线程退出时才把进程标 ZOMBIE。 */
int process_thread_exit_current(u32 code);

/* 线程 join：只接受同组、非当前、已经 ZOMBIE 的 TID；status 可写 0。 */
int process_thread_join_current(int tid, u32 user_status);

/* 线程 detach：标记同组 TID 为 detached；之后 join 返回 -1，退出由调度器自动回收。 */
int process_thread_detach_current(int tid);

int process_gettid_current(void);

/*
 * P4.2 exec：保留当前 PID/PPID/任务槽，新建 AS 并切换到内嵌用户镜像。
 * argv/envp 是旧 AS 中的用户指针；内核在释放旧 AS 前解析并复制字符串，
 * 按 argc/argv[]/NULL/envp[]/NULL/字符串区 写入新用户栈。
 * 成功返回 0，失败保留原 AS 并返回 -1；成功后调用方直接归还改写后的 irq_frame。
 * P4.3：exec 不触碰当前进程的 fd 表，pipe 端点随进程保留。
 */
int process_exec_current(struct irq_frame *frame, int image_id, u32 argv, u32 envp);

/* 标记当前进程 ZOMBIE 并记录退出码；调用方随后 task_exit()。 */
int process_exit_current(u32 code);

/*
 * 同步异常的唯一终止入口：把当前进程标成 ZOMBIE，exit status 记为
 * PROC_WAIT_STATUS(sig)，并给父进程置 SIGCHLD pending；调用方随后 task_exit()。
 */
int process_terminate_current(int sig);

/*
 * P3.2 每进程同步信号动作：handler=0 默认终止，1 忽略，其他值为用户 VA。
 * mask 只随 signal frame 保存/恢复，当前不做阻塞。
 */
int process_sigaction_current(int sig, u32 handler, u32 mask);

/*
 * 用户态同步异常投递：有用户 handler 时在用户栈构造 signal frame，
 * 改写 iret 目标；返回 1 表示 frame 已改成 handler 入口。
 * 无 handler、忽略但无法安全前移、或 frame 构造失败时返回 0，由调用方默认终止。
 * #DE 的保存 EIP 指向 faulting div/idiv，本函数会按最小指令长度前移。
 */
int process_signal_deliver(struct irq_frame *frame, int sig, u32 vector);

/* sigreturn：从用户栈 signal frame 恢复原 GPR/EIP/CS/EFLAGS/ESP/SS。 */
int process_sigreturn_current(struct irq_frame *frame);

/* 当前进程已执行 handler 的次数；异常记录在投递时取一次快照，避免 PID 复用后失真。 */
u32 process_sig_handled_current(void);

/* wait 子进程：P7.5 用 BLOCKED/WAKE 等待，找到 ZOMBIE 后写 user status 并回收。
 * pid>0 只等指定直接子进程，不是当前进程的子进程立即返回 -1；
 * pid<=0 等任意直接子进程。 */
int process_wait(int pid, u32 user_status);

/* P11.1：带 PIT tick 超时的 wait；返回子 PID，超时返回 0，错误返回 -1。
 * ticks=0 只做一次非阻塞查询，语义接近 WNOHANG。 */
int process_wait_timeout(int pid, u32 user_status, u32 ticks);

/* 最近一次 orphan/reparent 事件；没有发生过返回 0。 */
int process_last_orphan(struct proc_orphan_event *out);

/*
 * P7.5：调度器在任务真正转 ZOMBIE 后调用。先唤醒同组 join 等待者；
 * 若进程已整体 ZOMBIE，再唤醒父进程的 wait 等待者。
 */
void process_notify_task_zombie(struct process *proc);

/* P7.5 wait/join 阻塞计数：blocks 为实际进入 BLOCKED 的次数，wakes 为被唤醒返回次数。 */
void process_block_stats(u32 *wait_blocks, u32 *wait_wakes,
                         u32 *join_blocks, u32 *join_wakes);

int process_getpid_current(void);
int process_getppid_current(void);
int process_alive(int pid);
int process_live_count(void);

/* V 停止：给所有活进程的控制页写停止字。 */
void process_stop_all(void);

/* V 新启动前：回收上一轮 ZOMBIE；P10 后回收即清零 PCB。 */
void process_cleanup(void);

/* 调度器钩子：回收没有活父进程的 ZOMBIE；不碰当前任务。 */
void process_reap_orphans(void);

int process_snapshot(int slot, struct process_info *out);
int process_count(void);
/* PROC_MAX 是静态 PCB 槽容量；LIVE/ZOMBIE 占用槽，回收后直接 FREE。 */
int process_slot_used(void);
int process_pid_of(struct process *proc);
struct address_space *process_space(struct process *proc);
/* 只读观察入口：按 PCB 槽取 AS/fd，不转移所有权；FREE 槽返回空。 */
struct address_space *process_slot_space(int slot);
int process_slot_fd_info(int slot, int fd, struct proc_fd_info *out);
/* 每进程 fd 表归 kernel/proc/fd；exec 不重建它，fork 复制引用。 */
struct proc_fd_table *process_fds(struct process *proc);
u32 process_read32(int pid, u32 va);
/* exec 后镜像编号；0 表示初始 ring3 镜像，1 表示 P4.2 exec 镜像，2 表示 P4.3 pipe 镜像。 */
int process_image_id(int pid);
/* exec 后参数计数；优先读活 PCB，否则读同 PID 的最新 exit ring，失败返回 -1。 */
int process_exec_counts(int pid, u32 *argc, u32 *envc);

/* 最近两次 wait 回收子进程的 exec 快照，用于 PID 复用后的 V 页证据。
 * 数据来自全局 exit ring；slot 0=较早一条，slot 1=最新一条。 */
int process_reaped_exec(int parent_pid, int slot, int *pid,
                        u32 *image_id, u32 *argc, u32 *envc);

/* 读全局 exit ring。index 0=最新，index 1=次新；失败返回 -1。 */
int process_exit_snapshot(int index, struct process_exit_info *out);

#endif
