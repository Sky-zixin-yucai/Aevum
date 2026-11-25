#include "app/user/pages.h"
#include "app/user/page.h"
#include "app/user/render/draw.h"
#include "kernel/proc.h"
#include "kernel/proc/fd.h"
#include "kernel/ipc/pipe.h"
#include "arch/x86/exception.h"
#include "arch/x86/page_fault.h"
#include "drivers/video/vga.h"

static void fd_row(int slot, int fd, int row)
{
    struct proc_fd_info f;
    int used = process_slot_fd_info(slot, fd, &f);

    vga_text(2, row, "FD", C_GRAY);
    ud_put_dec(5, row, (u32)fd, C_WHITE);
    vga_text(9, row, "KIND", C_GRAY);
    vga_text(14, row, ud_fd_kind(used ? f.kind : 0), used ? C_CYAN : C_GRAY);
    vga_text(20, row, used ? "OPEN" : "----", used ? C_GREEN : C_GRAY);
    if (used) {
        vga_text(26, row, "kind", C_GRAY);
        ud_put_dec(31, row, f.kind, C_WHITE);
    }
}

void ur_render_fd(void)
{
    struct process_info p;
    struct exception_info x;
    struct page_fault_info pf;
    int slot = user_page_selected_slot();
    int i, used_fd = 0, pipe_fd = 0;
    int kids_live = 0, kids_zomb = 0, kids_dead = 0;
    int have_x, have_pf;

    if (!process_snapshot(slot, &p)) {
        vga_text(2, 3, "FD/SIG slot", C_GRAY);
        ud_put_dec(14, 3, (u32)slot, C_WHITE);
        vga_text(17, 3, "EMPTY; UP/DOWN selects another PCB slot", C_YELLOW);
        return;
    }

    vga_text(2, 3, "FD/SIG slot", C_GRAY);
    ud_put_dec(14, 3, (u32)slot, C_WHITE);
    vga_text(17, 3, "PID", C_GRAY);
    ud_put_dec(21, 3, (u32)p.pid, C_WHITE);
    vga_text(24, 3, p.comm, C_HDR);

    vga_text(2, 4, "FD", C_GRAY);
    vga_text(9, 4, "KIND", C_GRAY);
    vga_text(20, 4, "STATE", C_GRAY);

    for (i = 0; i < PROC_FD_MAX; i++) {
        struct proc_fd_info f;
        if (process_slot_fd_info(slot, i, &f)) {
            used_fd++;
            if (f.kind == PROC_FD_PIPE_READ || f.kind == PROC_FD_PIPE_WRITE)
                pipe_fd++;
        }
        fd_row(slot, i, 5 + i);
    }

    vga_text(2, 13, "OPEN fd", C_GRAY);
    ud_put_dec(10, 13, (u32)used_fd, C_WHITE);
    vga_cell(12, 13, '/', C_GRAY);
    ud_put_dec(13, 13, (u32)PROC_FD_MAX, C_CYAN);
    vga_text(16, 13, "PIPE endpoints", C_GRAY);
    ud_put_dec(31, 13, (u32)pipe_fd, pipe_fd ? C_YELLOW : C_GRAY);
    vga_text(35, 13, "fd table is per process; exec keeps it", C_GRAY);

    vga_text(2, 14, "SIG pending", C_GRAY);
    ud_put_hex(14, 14, p.pending, p.pending ? C_MAG : C_GRAY);
    vga_text(23, 14, "last", C_GRAY);
    ud_put_dec(28, 14, p.last_sig, p.last_sig ? C_MAG : C_GRAY);
    vga_text(31, 14, "term", C_GRAY);
    ud_put_dec(36, 14, p.term_sig, p.term_sig ? C_MAG : C_GRAY);
    vga_text(39, 14, "count", C_GRAY);
    ud_put_dec(45, 14, p.sig_count, C_WHITE);
    vga_text(48, 14, "chld", C_GRAY);
    ud_put_dec(53, 14, p.sigchld_count, C_CYAN);
    vga_text(56, 14, "hdl", C_GRAY);
    ud_put_dec(60, 14, p.sig_handled, p.sig_handled ? C_GREEN : C_GRAY);
    vga_text(63, 14, "mask", C_GRAY);
    ud_put_hex(68, 14, p.sig_mask, C_WHITE);

    have_x = exception_last(&x);
    vga_text(2, 15, "EXC", C_GRAY);
    if (have_x) {
        vga_text(6, 15, exception_vector_name(x.vector), C_MAG);
        vga_text(18, 15, "cnt", C_GRAY);
        ud_put_dec(22, 15, exception_vector_count(x.vector), C_WHITE);
        vga_text(26, 15, "pid", C_GRAY);
        ud_put_dec(30, 15, x.pid, C_WHITE);
        vga_text(33, 15, "eip", C_GRAY);
        ud_put_hex(37, 15, x.eip, C_CYAN);
        vga_text(46, 15, "sig", C_GRAY);
        ud_put_dec(50, 15, x.sig, C_MAG);
        vga_text(53, 15, exception_signal_name(x.sig), C_MAG);
        vga_text(64, 15, "hdl", C_GRAY);
        ud_put_dec(68, 15, x.handled, x.handled ? C_GREEN : C_YELLOW);
    } else {
        vga_text(6, 15, "none yet", C_GRAY);
    }

    have_pf = page_fault_last(&pf);
    vga_text(2, 16, "#PF", C_GRAY);
    if (have_pf) {
        vga_text(6, 16, "pid", C_GRAY);
        ud_put_dec(10, 16, pf.pid, C_WHITE);
        vga_text(13, 16, "eip", C_GRAY);
        ud_put_hex(17, 16, pf.eip, C_CYAN);
        vga_text(26, 16, "cr2", C_GRAY);
        ud_put_hex(30, 16, pf.cr2, C_WHITE);
        vga_text(39, 16, "err", C_GRAY);
        ud_put_hex(43, 16, pf.err, C_MAG);
        vga_text(52, 16, "count", C_GRAY);
        ud_put_dec(58, 16, page_fault_count(), C_WHITE);
    } else {
        vga_text(6, 16, "none yet", C_GRAY);
    }

    vga_text(2, 17, "PIPE", C_GRAY);
    vga_text(7, 17, "obj", C_GRAY);
    ud_put_dec(11, 17, pipe_live_count(), pipe_live_count() ? C_YELLOW : C_GREEN);
    vga_text(15, 17, "selected endpoints", C_GRAY);
    ud_put_dec(34, 17, (u32)pipe_fd, pipe_fd ? C_YELLOW : C_GRAY);
    vga_text(37, 17, "PIPE_MAX 4 / CAP 64", C_GRAY);

    for (i = 0; i < process_count(); i++) {
        struct process_info c;
        if (!process_snapshot(i, &c) || c.ppid != p.pid) continue;
        if (c.state == PROC_LIVE) kids_live++;
        else if (c.state == PROC_ZOMBIE) kids_zomb++;
        else kids_dead++;
    }

    vga_text(2, 18, "WAIT children live", C_GRAY);
    ud_put_dec(21, 18, (u32)kids_live, C_GREEN);
    vga_text(24, 18, "zomb", C_GRAY);
    ud_put_dec(29, 18, (u32)kids_zomb, kids_zomb ? C_MAG : C_GRAY);
    vga_text(33, 18, "dead", C_GRAY);
    ud_put_dec(38, 18, (u32)kids_dead, C_GRAY);
    vga_text(43, 18, "WAITER", C_GRAY);
    vga_text(50, 18, kids_zomb ? "YES" : "NO", kids_zomb ? C_GREEN : C_GRAY);

    vga_text(2, 19, "SIG names: pending bits 0..31; SIGCHLD=17; handled is user frame count", C_GRAY);
    vga_text(2, 20, "FD kinds: CONR/CONW console, PIPR/PIPW pipe, RAMF ramfs; CLOSED ----", C_GRAY);
}
