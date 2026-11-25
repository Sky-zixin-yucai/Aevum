#include "app/user/render/draw.h"
#include "kernel/task.h"
#include "kernel/proc/fd.h"
#include "kernel/mm/address_space.h"
#include "drivers/video/vga.h"
#include "lib/format.h"

int ud_put_dec(int col, int row, u32 value, u8 attr)
{
    char buf[12];
    int n = fmt_dec(buf, value);

    buf[n] = 0;
    vga_text(col, row, buf, attr);
    return col + n;
}

int ud_put_hex(int col, int row, u32 value, u8 attr)
{
    char buf[12];
    int n = fmt_hex_width(buf, value, 8);

    buf[n] = 0;
    vga_text(col, row, buf, attr);
    return col + n;
}

const char *ud_proc_state(const struct process_info *p)
{
    if (p->state == PROC_DEAD) {
        /* PID 会被 fork 子进程复用，角色只能看 comm，不能拿旧 lane PID 比对。 */
        if (p->term_sig == PROC_SIGSEGV && p->comm[4] == 'C') return "FAULT";
        if (p->term_sig == PROC_SIGSEGV && p->comm[4] == 'F') return "GUARD";
        if (p->term_sig) return "SIG";
        return "REAP";
    }
    if (p->state == PROC_ZOMBIE) return "ZOMB";
    return task_state_name(p->task_state);
}

u8 ud_proc_attr(const struct process_info *p)
{
    if (p->state == PROC_DEAD) {
        if (p->term_sig) return C_MAG;
        return C_GRAY;
    }
    if (p->state == PROC_ZOMBIE) return C_MAG;
    if (p->task_state == TASK_RUNNING) return C_GREEN;
    if (p->task_state == TASK_READY) return C_YELLOW;
    return C_GRAY;
}

const char *ud_vma_kind(u32 kind)
{
    switch (kind) {
    case ADDRESS_SPACE_VMA_CODE:   return "CODE";
    case ADDRESS_SPACE_VMA_DATA:   return "DATA";
    case ADDRESS_SPACE_VMA_GUARD:  return "GUARD";
    case ADDRESS_SPACE_VMA_STACK:  return "STACK";
    case ADDRESS_SPACE_VMA_CONTROL:return "CTRL";
    case ADDRESS_SPACE_VMA_HEAP:   return "HEAP";
    default: return "?";
    }
}

const char *ud_fd_kind(u32 kind)
{
    switch (kind) {
    case PROC_FD_PIPE_READ:    return "PIPR";
    case PROC_FD_PIPE_WRITE:   return "PIPW";
    case PROC_FD_CONSOLE_READ: return "CONR";
    case PROC_FD_CONSOLE_WRITE:return "CONW";
    case PROC_FD_RAMFS_FILE:   return "RAMF";
    default:                   return "----";
    }
}
