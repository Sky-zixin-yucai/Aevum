#include "app/user/pages.h"
#include "app/user/page.h"
#include "app/user/render/draw.h"
#include "kernel/proc.h"
#include "kernel/task.h"
#include "kernel/task/stack.h"
#include "kernel/mm/address_space.h"
#include "kernel/ipc/pipe.h"
#include "arch/x86/syscall.h"
#include "drivers/video/vga.h"

static void slot_row(int slot, int row)
{
    struct process_info p;

    if (slot == user_page_selected_slot())
        vga_cell(0, row, '>', C_GREEN);

    if (!process_snapshot(slot, &p)) {
        vga_text(2, row, "SLOT", C_GRAY);
        ud_put_dec(7, row, (u32)slot, C_GRAY);
        vga_text(25, row, "FREE", C_GRAY);
        return;
    }

    vga_text(2, row, "SLOT", C_GRAY);
    ud_put_dec(7, row, (u32)slot, C_WHITE);
    vga_text(9, row, "PID", C_GRAY);
    ud_put_dec(13, row, (u32)p.pid, C_WHITE);
    vga_text(16, row, "PP", C_GRAY);
    ud_put_dec(19, row, (u32)p.ppid, C_WHITE);
    vga_text(22, row, "ST", C_GRAY);
    vga_text(25, row, ud_proc_state(&p), ud_proc_attr(&p));
    vga_text(31, row, "TK", C_GRAY);
    vga_text(34, row, task_state_name(p.task_state), ud_proc_attr(&p));
    vga_text(42, row, "EX", C_GRAY);
    if (p.state == PROC_ZOMBIE || p.state == PROC_DEAD) {
        if (p.term_sig) {
            vga_text(45, row, "S", C_MAG);
            ud_put_dec(46, row, p.term_sig, C_MAG);
        } else {
            ud_put_dec(45, row, p.exit_code, C_WHITE);
        }
    } else {
        vga_text(45, row, "-", C_GRAY);
    }
    vga_text(49, row, "ASID", C_GRAY);
    ud_put_dec(54, row, p.asid, C_WHITE);
    vga_text(57, row, "PGS", C_GRAY);
    ud_put_dec(61, row, p.user_pages, C_WHITE);
    vga_cell(63, row, '/', C_GRAY);
    ud_put_dec(64, row, p.cow_pages, p.cow_pages ? C_MAG : C_GRAY);
    vga_text(66, row, p.comm, C_HDR);
}

static void selected_detail(int slot)
{
    struct process_info p;
    struct process_exit_info e;
    int i;
    int kids_live = 0, kids_zomb = 0, kids_dead = 0;
    int live = 0, zomb = 0, dead = 0;

    for (i = 0; i < process_count(); i++) {
        struct process_info cur;
        if (!process_snapshot(i, &cur)) continue;
        if (cur.state == PROC_LIVE) live++;
        else if (cur.state == PROC_ZOMBIE) zomb++;
        else dead++;
    }

    if (!process_snapshot(slot, &p)) {
        vga_text(2, 14, "SELECTED slot", C_GRAY);
        ud_put_dec(16, 14, (u32)slot, C_WHITE);
        vga_text(19, 14, "EMPTY; use UP/DOWN to a used PCB slot", C_YELLOW);
        goto totals;
    }

    for (i = 0; i < process_count(); i++) {
        struct process_info cur;
        if (!process_snapshot(i, &cur)) continue;
        if (cur.ppid != p.pid) continue;
        if (cur.state == PROC_LIVE) kids_live++;
        else if (cur.state == PROC_ZOMBIE) kids_zomb++;
        else kids_dead++;
    }

    vga_text(2, 14, "SELECTED slot", C_GRAY);
    ud_put_dec(16, 14, (u32)slot, C_WHITE);
    vga_text(19, 14, "PID", C_GRAY);
    ud_put_dec(23, 14, (u32)p.pid, C_WHITE);
    vga_text(26, 14, "PP", C_GRAY);
    ud_put_dec(29, 14, (u32)p.ppid, C_WHITE);
    vga_text(32, 14, "COMM", C_GRAY);
    vga_text(37, 14, p.comm, C_HDR);

    vga_text(2, 15, "TASK", C_GRAY);
    vga_text(7, 15, task_state_name(p.task_state), ud_proc_attr(&p));
    vga_text(17, 15, "KSTACK TOP", C_GRAY);
    ud_put_hex(28, 15, p.stack_top, C_CYAN);
    vga_text(38, 15, "KSTK", C_GRAY);
    ud_put_dec(43, 15, (u32)kernel_stack_used(), C_WHITE);
    vga_cell(45, 15, '/', C_GRAY);
    ud_put_dec(46, 15, (u32)kernel_stack_capacity(), C_CYAN);

    vga_text(2, 16, "AS CR3", C_GRAY);
    ud_put_hex(9, 16, p.cr3, C_CYAN);
    vga_text(18, 16, "ASID", C_GRAY);
    ud_put_dec(23, 16, p.asid, C_WHITE);
    vga_text(27, 16, "PGS/C", C_GRAY);
    ud_put_dec(33, 16, p.user_pages, C_WHITE);
    vga_cell(35, 16, '/', C_GRAY);
    ud_put_dec(36, 16, p.cow_pages, p.cow_pages ? C_MAG : C_GRAY);
    vga_text(39, 16, "BRK", C_GRAY);
    ud_put_hex(43, 16, p.brk, C_YELLOW);
    vga_text(52, 16, "VMA", C_GRAY);
    ud_put_dec(56, 16, p.vma_count, C_WHITE);

    vga_text(2, 17, "IMAGE", C_GRAY);
    ud_put_dec(8, 17, p.image_id, p.image_id ? C_CYAN : C_WHITE);
    vga_text(11, 17, "arg", C_GRAY);
    ud_put_dec(15, 17, p.exec_argc, p.exec_argc ? C_CYAN : C_GRAY);
    vga_text(18, 17, "env", C_GRAY);
    ud_put_dec(22, 17, p.exec_envc, p.exec_envc ? C_CYAN : C_GRAY);
    vga_text(26, 17, "switch", C_GRAY);
    ud_put_dec(33, 17, p.switches, C_WHITE);
    vga_text(40, 17, "ticks", C_GRAY);
    ud_put_dec(46, 17, p.cpu_ticks, C_WHITE);
    vga_text(52, 17, "THR", C_GRAY);
    ud_put_dec(56, 17, (u32)p.thread_count,
               p.thread_count > 1 ? C_MAG : C_WHITE);
    vga_text(59, 17, "TID", C_GRAY);
    ud_put_dec(63, 17, (u32)p.tid, p.tid ? C_CYAN : C_GRAY);

    vga_text(2, 18, "SIG pend", C_GRAY);
    ud_put_hex(11, 18, p.pending, p.pending ? C_MAG : C_GRAY);
    vga_text(20, 18, "last", C_GRAY);
    ud_put_dec(25, 18, p.last_sig, p.last_sig ? C_MAG : C_GRAY);
    vga_text(28, 18, "term", C_GRAY);
    ud_put_dec(33, 18, p.term_sig, p.term_sig ? C_MAG : C_GRAY);
    vga_text(36, 18, "count", C_GRAY);
    ud_put_dec(42, 18, p.sig_count, C_WHITE);
    vga_text(45, 18, "chld", C_GRAY);
    ud_put_dec(50, 18, p.sigchld_count, C_CYAN);
    vga_text(53, 18, "hdl", C_GRAY);
    ud_put_dec(57, 18, p.sig_handled, p.sig_handled ? C_GREEN : C_GRAY);
    vga_text(60, 18, "mask", C_GRAY);
    ud_put_hex(65, 18, p.sig_mask, C_WHITE);

    vga_text(2, 19, "CHILDREN live", C_GRAY);
    ud_put_dec(15, 19, (u32)kids_live, C_GREEN);
    vga_text(18, 19, "zomb", C_GRAY);
    ud_put_dec(23, 19, (u32)kids_zomb, kids_zomb ? C_MAG : C_GRAY);
    vga_text(27, 19, "dead", C_GRAY);
    ud_put_dec(32, 19, (u32)kids_dead, C_GRAY);
    vga_text(37, 19, "WAITER", C_GRAY);
    vga_text(44, 19, kids_zomb ? "YES" : "NO", kids_zomb ? C_GREEN : C_GRAY);

totals:
    vga_text(2, 20, "POOL proc", C_GRAY);
    ud_put_dec(12, 20, (u32)(live + zomb + dead), C_WHITE);
    vga_cell(14, 20, '/', C_GRAY);
    ud_put_dec(15, 20, (u32)process_count(), C_CYAN);
    vga_text(18, 20, "live", C_GRAY);
    ud_put_dec(23, 20, (u32)live, C_GREEN);
    vga_text(26, 20, "zomb", C_GRAY);
    ud_put_dec(31, 20, (u32)zomb, zomb ? C_MAG : C_GRAY);
    vga_text(34, 20, "dead", C_GRAY);
    ud_put_dec(39, 20, (u32)dead, C_GRAY);
    vga_text(43, 20, "task", C_GRAY);
    ud_put_dec(48, 20, (u32)task_slot_used(), C_WHITE);
    vga_cell(50, 20, '/', C_GRAY);
    ud_put_dec(51, 20, (u32)task_slot_capacity(), C_CYAN);
    vga_text(54, 20, "AS", C_GRAY);
    ud_put_dec(57, 20, (u32)address_space_slot_used(), C_WHITE);
    vga_cell(59, 20, '/', C_GRAY);
    ud_put_dec(60, 20, (u32)address_space_slot_capacity(), C_CYAN);

    vga_text(2, 21, "PHYS", C_GRAY);
    ud_put_dec(7, 21, address_space_phys_used(), C_WHITE);
    vga_cell(11, 21, '/', C_GRAY);
    ud_put_dec(12, 21, address_space_phys_total(), C_CYAN);
    vga_text(17, 21, "syscall", C_GRAY);
    ud_put_dec(25, 21, syscall_count_total(), C_WHITE);
    vga_text(32, 21, "pipe obj", C_GRAY);
    ud_put_dec(41, 21, pipe_live_count(), pipe_live_count() ? C_YELLOW : C_GREEN);
    /* P10：PCB 清零后，最近一次退出仍可从 exit ring 读取。 */
    vga_text(52, 21, "exit", C_GRAY);
    if (process_exit_snapshot(0, &e) == 0) {
        ud_put_dec(57, 21, (u32)e.pid, C_WHITE);
        vga_text(60, 21, "st", C_GRAY);
        if (e.term_sig) {
            vga_text(63, 21, "S", C_MAG);
            ud_put_dec(64, 21, e.term_sig, C_MAG);
        } else {
            ud_put_dec(63, 21, e.exit_code, C_WHITE);
        }
    } else {
        vga_text(57, 21, "-", C_GRAY);
    }

    vga_text(2, 22, "SCROLL", C_GRAY);
    {
        int c = ud_put_dec(9, 22, (u32)(user_page_first_slot() + 1), C_WHITE);
        vga_cell(c, 22, '-', C_GRAY);
        c = ud_put_dec(c + 1, 22, (u32)(user_page_first_slot() + USER_PAGE_ROWS), C_WHITE);
        vga_cell(c, 22, '/', C_GRAY);
        c = ud_put_dec(c + 1, 22, (u32)PROC_MAX, C_CYAN);
        vga_text(c + 2, 22, "TAB next  PGUP/PGDN window  HOME/END", C_GRAY);
    }
}

void ur_render_slots(void)
{
    int i;

    vga_text(2, 3, "SLOT", C_GRAY);
    vga_text(9, 3, "PID", C_GRAY);
    vga_text(16, 3, "PP", C_GRAY);
    vga_text(22, 3, "ST", C_GRAY);
    vga_text(31, 3, "TK", C_GRAY);
    vga_text(42, 3, "EX", C_GRAY);
    vga_text(49, 3, "ASID", C_GRAY);
    vga_text(57, 3, "PGS", C_GRAY);
    vga_text(66, 3, "COMM", C_GRAY);

    for (i = 0; i < USER_PAGE_ROWS; i++)
        slot_row(user_page_first_slot() + i, 4 + i);

    selected_detail(user_page_selected_slot());
}
