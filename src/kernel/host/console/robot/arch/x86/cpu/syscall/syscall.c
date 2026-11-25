#include "arch/x86/syscall.h"
#include "arch/x86/interrupt.h"
#include "kernel/task.h"
#include "kernel/proc.h"
#include "kernel/proc/fd.h"
#include "kernel/mm/address_space.h"

extern void int80_stub(void);

static u32 call_count;
static u32 last_cs;

static struct address_space *current_space(void)
{
    struct process *proc = task_process_current();
    return proc ? process_space(proc) : 0;
}

static struct proc_fd_table *current_fds(void)
{
    struct process *proc = task_process_current();
    return proc ? process_fds(proc) : 0;
}

void syscall_init(void)
{
    interrupt_set_user_gate(0x80, int80_stub);
}

u32 syscall_dispatch(struct irq_frame *frame)
{
    u32 nr = frame->eax;
    call_count++;
    last_cs = frame->cs;
    switch (frame->eax) {
        case 2:
            frame->eax = 0;
            (void)task_yield();
            break;
        case 3:
            (void)process_exit_current(frame->ebx);
            task_exit();
        case 4:
            frame->eax = (u32)process_wait((int)frame->ebx, frame->ecx);
            break;
        case 5:
            frame->eax = (u32)process_getpid_current();
            break;
        case 6:
            {
                struct process *child = process_fork(frame);
                frame->eax = child ? (u32)process_pid_of(child) : (u32)-1;
            }
            break;
        case 7:
            frame->eax = (u32)process_getppid_current();
            break;
        case 8:
            {
                struct address_space *space = current_space();
                frame->eax = (space && address_space_set_brk(space, frame->ebx) == 0)
                                 ? address_space_brk(space)
                                 : (u32)-1;
            }
            break;
        case 9:
            {
                u32 value;
                struct address_space *space = current_space();
                frame->eax = (space &&
                              address_space_copyin(space, &value, frame->ebx, 4) == 0)
                                 ? value
                                 : (u32)-1;
            }
            break;
        case 10:
            {
                struct address_space *space = current_space();
                frame->eax = (space &&
                              address_space_user_write32(space, frame->ebx,
                                                         frame->ecx))
                                 ? 1u
                                 : 0u;
            }
            break;
        case 11:
            frame->eax = (process_sigaction_current((int)frame->ebx,
                                                    frame->ecx,
                                                    frame->edx) == 0)
                             ? 0u
                             : (u32)-1;
            break;
        case 12:
            if (process_sigreturn_current(frame) != 0)
                frame->eax = (u32)-1;
            break;
        case 13:
            frame->eax = (process_exec_current(frame, (int)frame->ebx,
                                               frame->ecx, frame->edx) == 0)
                             ? 0u
                             : (u32)-1;
            break;
        case 14:
            frame->eax = (proc_fd_pipe(current_fds(), current_space(),
                                       frame->ebx) == 0)
                             ? 0u
                             : (u32)-1;
            break;
        case 15:
            frame->eax = (u32)proc_fd_read(current_fds(), current_space(),
                                           frame->ebx, frame->ecx, frame->edx);
            break;
        case 16:
            frame->eax = (u32)proc_fd_write(current_fds(), current_space(),
                                            frame->ebx, frame->ecx, frame->edx);
            break;
        case 17:
            frame->eax = (proc_fd_close(current_fds(), frame->ebx) == 0)
                             ? 0u
                             : (u32)-1;
            break;
        case 18:
            {
                struct process *child = 0;
                if ((frame->cs & 3u) == 3u) {
                    child = process_spawn_current(1, "spawn", (int)frame->ebx,
                                                  frame->ecx, frame->edx);
                }
                frame->eax = child ? (u32)process_pid_of(child) : (u32)-1;
            }
            break;
        case 19:
            {
                struct process *self = task_process_current();
                int tid = -1;
                if ((frame->cs & 3u) == 3u && self) {
                    tid = process_clone_thread_current("userT",
                                                       frame->ebx, frame->ecx,
                                                       frame->edx, frame->esi);
                }
                frame->eax = (u32)tid;
            }
            break;
        case 20:
            (void)process_thread_exit_current(frame->ebx);
            task_exit();
        case 21:
            frame->eax = (u32)process_gettid_current();
            break;
        case 22: {
            int rc = process_thread_join_current((int)frame->ebx,
                                                 frame->ecx);
            /* P11.7：EINTR 必须透传 -3，不能被压成普通错误 -1。 */
            frame->eax = (rc == 0) ? 0u : (rc == -3 ? (u32)-3 : (u32)-1);
            break;
        }
        case 23:
            frame->eax = (process_thread_detach_current((int)frame->ebx) == 0)
                             ? 0u
                             : (u32)-1;
            break;
        case 24:
            frame->eax = (u32)task_sleep_ticks(frame->ebx);
            break;
        case 25:
            frame->eax = (u32)process_wait_timeout((int)frame->ebx,
                                                   frame->ecx, frame->edx);
            break;
        case 26:
            frame->eax = (u32)task_priority_get((int)frame->ebx);
            break;
        case 27:
            frame->eax = (u32)task_priority_set((int)frame->ebx,
                                                (int)frame->ecx);
            break;
        case 28:
            frame->eax = (u32)proc_fd_read_block(current_fds(), current_space(),
                                                 frame->ebx, frame->ecx,
                                                 frame->edx, frame->esi);
            break;
        case 29:
            frame->eax = (u32)proc_fd_write_block(current_fds(), current_space(),
                                                  frame->ebx, frame->ecx,
                                                  frame->edx, frame->esi);
            break;
        case 30:
            frame->eax = (u32)process_signal_send((int)frame->ebx,
                                                  (int)frame->ecx);
            break;
        case 31:
            frame->eax = (process_sigaction_flags_current((int)frame->ebx,
                                                          frame->ecx,
                                                          frame->edx,
                                                          frame->esi) == 0)
                             ? 0u
                             : (u32)-1;
            break;
        default:
            frame->eax = (u32)-1;
            break;
    }
    /*
     * P11.8：阻塞 syscall 的 -3 在这里先准备 SA_RESTART，再在 int80 返回前
     * 由 process_signal_deliver_async() 投递 handler。nr 是进入时的系统调用号。
     */
    if ((int)frame->eax == -3)
        process_signal_restart_prepare(frame, nr);
    return (u32)frame;
}

u32 syscall_count_total(void)
{
    return call_count;
}

u32 syscall_last_cs(void)
{
    return last_cs;
}
