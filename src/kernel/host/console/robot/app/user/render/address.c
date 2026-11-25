#include "app/user/pages.h"
#include "app/user/page.h"
#include "app/user/render/draw.h"
#include "app/user/demo.h"
#include "kernel/proc.h"
#include "kernel/task.h"
#include "kernel/task/stack.h"
#include "kernel/mm/address_space.h"
#include "drivers/video/vga.h"

static void vma_row(const struct address_space *space, int slot, int row)
{
    struct address_space_vma_info v;
    int rd, wr;
    u32 c = 47;

    if (!address_space_vma_snapshot(space, slot, &v)) {
        vga_text(2, row, "-", C_GRAY);
        return;
    }

    rd = address_space_user_range_ok(space, v.start, 1, 0);
    wr = address_space_user_range_ok(space, v.start, 1, 1);

    ud_put_dec(2, row, (u32)slot, C_WHITE);
    vga_text(6, row, ud_vma_kind(v.kind), v.kind == ADDRESS_SPACE_VMA_GUARD ? C_MAG : C_CYAN);
    vga_text(12, row, "START", C_GRAY);
    ud_put_hex(18, row, v.start, C_WHITE);
    vga_text(27, row, "END", C_GRAY);
    ud_put_hex(31, row, v.end, C_WHITE);
    vga_text(40, row, "FLAGS", C_GRAY);
    vga_cell(c, row, (v.flags & ADDRESS_SPACE_VMA_READ) ? 'R' : '-',
             (v.flags & ADDRESS_SPACE_VMA_READ) ? C_GREEN : C_GRAY);
    vga_cell(c + 1, row, (v.flags & ADDRESS_SPACE_VMA_WRITE) ? 'W' : '-',
             (v.flags & ADDRESS_SPACE_VMA_WRITE) ? C_YELLOW : C_GRAY);
    vga_cell(c + 2, row, (v.flags & ADDRESS_SPACE_VMA_EXEC) ? 'X' : '-',
             (v.flags & ADDRESS_SPACE_VMA_EXEC) ? C_MAG : C_GRAY);
    vga_text(51, row, "PTE", C_GRAY);
    vga_cell(55, row, rd ? 'R' : '-', rd ? C_GREEN : C_GRAY);
    vga_cell(56, row, wr ? 'W' : '-', wr ? C_GREEN : C_MAG);
}

void ur_render_address(void)
{
    struct process_info p;
    struct task_info ti;
    struct address_space *space;
    struct address_space_cow_info ci;
    int slot = user_page_selected_slot();
    int i, n;
    int hpid0 = 0, hpid1 = 0;
    u32 himg0 = 0, harg0 = 0, henp0 = 0;
    u32 himg1 = 0, harg1 = 0, henp1 = 0;
    int r0 = -1, r1 = -1;
    u32 wait_magic, cow_ok, sig_ok, exec_ok, pipe_ok, spawn_ok, thread_ok, det_ok;
    u32 block_ok, timer_ok, priority_ok, wait_blocks, wait_wakes, join_blocks, join_wakes;
    u32 brk, poke, peek, guard, bad;

    if (!process_snapshot(slot, &p)) {
        vga_text(2, 3, "ADDRESS slot", C_GRAY);
        ud_put_dec(15, 3, (u32)slot, C_WHITE);
        vga_text(18, 3, "EMPTY; UP/DOWN selects another PCB slot", C_YELLOW);
        return;
    }

    space = process_slot_space(slot);
    n = address_space_vma_count(space);

    vga_text(2, 3, "ADDRESS slot", C_GRAY);
    ud_put_dec(15, 3, (u32)slot, C_WHITE);
    vga_text(18, 3, "PID", C_GRAY);
    ud_put_dec(22, 3, (u32)p.pid, C_WHITE);
    vga_text(25, 3, p.comm, C_HDR);

    vga_text(2, 4, "CR3", C_GRAY);
    ud_put_hex(7, 4, p.cr3, C_CYAN);
    vga_text(16, 4, "ASID", C_GRAY);
    ud_put_dec(21, 4, p.asid, C_WHITE);
    vga_text(26, 4, "VMA", C_GRAY);
    ud_put_dec(30, 4, (u32)n, C_WHITE);
    vga_text(33, 4, "PGS/C", C_GRAY);
    ud_put_dec(39, 4, p.user_pages, C_WHITE);
    vga_cell(42, 4, '/', C_GRAY);
    ud_put_dec(43, 4, p.cow_pages, p.cow_pages ? C_MAG : C_GRAY);
    vga_text(47, 4, "BRK", C_GRAY);
    ud_put_hex(51, 4, p.brk, C_YELLOW);

    address_space_cow_snapshot(&ci);
    vga_text(2, 5, "PHYS", C_GRAY);
    vga_text(7, 5, "used", C_GRAY);
    ud_put_dec(12, 5, address_space_phys_used(), C_WHITE);
    vga_text(15, 5, "total", C_GRAY);
    ud_put_dec(21, 5, address_space_phys_total(), C_CYAN);
    vga_text(25, 5, "COW live", C_GRAY);
    ud_put_dec(34, 5, ci.live, ci.live ? C_MAG : C_GRAY);
    vga_text(38, 5, "fork", C_GRAY);
    ud_put_dec(43, 5, ci.forks, C_WHITE);
    vga_text(47, 5, "share", C_GRAY);
    ud_put_dec(53, 5, ci.shared, C_CYAN);
    vga_text(57, 5, "break", C_GRAY);
    ud_put_dec(63, 5, ci.breaks, C_WHITE);

    vga_text(2, 6, "#", C_GRAY);
    vga_text(6, 6, "KIND", C_GRAY);
    vga_text(12, 6, "START", C_GRAY);
    vga_text(27, 6, "END", C_GRAY);
    vga_text(40, 6, "FLAGS", C_GRAY);
    vga_text(51, 6, "PTE", C_GRAY);

    for (i = 0; i < 6; i++) vma_row(space, i, 7 + i);

    vga_text(2, 13, "TASK", C_GRAY);
    vga_text(7, 13, task_state_name(p.task_state), ud_proc_attr(&p));
    if (task_snapshot_pid(p.pid, &ti)) {
        vga_text(14, 13, "P", C_GRAY);
        ud_put_dec(15, 13, (u32)ti.priority, C_YELLOW);
    }
    vga_text(18, 13, "KSTACK", C_GRAY);
    ud_put_hex(25, 13, p.stack_top, C_CYAN);
    vga_text(34, 13, "SW", C_GRAY);
    ud_put_dec(37, 13, p.switches, C_WHITE);
    vga_text(43, 13, "TICKS", C_GRAY);
    ud_put_dec(49, 13, p.cpu_ticks, C_WHITE);
    vga_text(53, 13, "KSTK", C_GRAY);
    ud_put_dec(58, 13, (u32)kernel_stack_used(), C_WHITE);
    vga_cell(60, 13, '/', C_GRAY);
    ud_put_dec(61, 13, (u32)kernel_stack_capacity(), C_CYAN);

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

    vga_text(2, 15, "EXEC image", C_GRAY);
    ud_put_dec(13, 15, p.image_id, p.image_id ? C_CYAN : C_WHITE);
    vga_text(16, 15, "argc", C_GRAY);
    ud_put_dec(21, 15, p.exec_argc, p.exec_argc ? C_CYAN : C_GRAY);
    vga_text(24, 15, "envc", C_GRAY);
    ud_put_dec(29, 15, p.exec_envc, p.exec_envc ? C_CYAN : C_GRAY);
    vga_text(34, 15, "THR", C_GRAY);
    ud_put_dec(38, 15, (u32)p.thread_count,
               p.thread_count > 1 ? C_MAG : C_WHITE);
    vga_text(41, 15, "TID", C_GRAY);
    ud_put_dec(45, 15, (u32)p.tid, p.tid ? C_CYAN : C_GRAY);
    process_block_stats(&wait_blocks, &wait_wakes, &join_blocks, &join_wakes);
    vga_text(48, 15, "blk", C_GRAY);
    ud_put_dec(52, 15, wait_blocks, wait_blocks ? C_GREEN : C_GRAY);
    vga_cell(54, 15, '/', C_GRAY);
    ud_put_dec(55, 15, join_blocks, join_blocks ? C_GREEN : C_GRAY);

    r0 = process_reaped_exec(p.pid, 0, &hpid0, &himg0, &harg0, &henp0);
    r1 = process_reaped_exec(p.pid, 1, &hpid1, &himg1, &harg1, &henp1);
    vga_text(2, 16, "REAP0", C_GRAY);
    if (r0 == 0) {
        ud_put_dec(8, 16, (u32)hpid0, C_WHITE);
        vga_text(11, 16, "img", C_GRAY);
        ud_put_dec(15, 16, himg0, C_CYAN);
        vga_text(18, 16, "arg", C_GRAY);
        ud_put_dec(22, 16, harg0, C_WHITE);
        vga_text(25, 16, "env", C_GRAY);
        ud_put_dec(29, 16, henp0, C_WHITE);
    } else {
        vga_text(8, 16, "-", C_GRAY);
    }
    vga_text(38, 16, "REAP1", C_GRAY);
    if (r1 == 0) {
        ud_put_dec(44, 16, (u32)hpid1, C_WHITE);
        vga_text(47, 16, "img", C_GRAY);
        ud_put_dec(51, 16, himg1, C_CYAN);
        vga_text(54, 16, "a", C_GRAY);
        ud_put_dec(56, 16, harg1, C_WHITE);
        vga_text(58, 16, "e", C_GRAY);
        ud_put_dec(60, 16, henp1, C_WHITE);
    } else {
        vga_text(44, 16, "-", C_GRAY);
    }

    vga_text(2, 17, "COW last ASID", C_GRAY);
    ud_put_dec(16, 17, ci.last_asid, C_WHITE);
    vga_text(20, 17, "old", C_GRAY);
    ud_put_hex(24, 17, ci.last_old, C_CYAN);
    vga_text(33, 17, "new", C_GRAY);
    ud_put_hex(37, 17, ci.last_new, C_GREEN);
    vga_text(46, 17, "break", C_GRAY);
    ud_put_dec(52, 17, ci.breaks, C_WHITE);

    vga_text(2, 18, "CHILDREN", C_GRAY);
    {
        int live = 0, zomb = 0, dead = 0;
        struct process_info c;
        for (i = 0; i < process_count(); i++) {
            if (!process_snapshot(i, &c) || c.ppid != p.pid) continue;
            if (c.state == PROC_LIVE) live++;
            else if (c.state == PROC_ZOMBIE) zomb++;
            else dead++;
        }
        vga_text(11, 18, "live", C_GRAY);
        ud_put_dec(16, 18, (u32)live, C_GREEN);
        vga_text(19, 18, "zomb", C_GRAY);
        ud_put_dec(24, 18, (u32)zomb, zomb ? C_MAG : C_GRAY);
        vga_text(28, 18, "dead", C_GRAY);
        ud_put_dec(33, 18, (u32)dead, C_GRAY);
        vga_text(38, 18, "WAITER", C_GRAY);
        vga_text(45, 18, zomb ? "YES" : "NO", zomb ? C_GREEN : C_GRAY);
    }

    vga_text(2, 19, "PTE R/W probes VMA start; GUARD has VMA without PTE", C_CYAN);
    vga_text(2, 20, "THREAD", C_GRAY);
    vga_text(9, 20, "tid", C_GRAY);
    ud_put_dec(13, 20, user_demo_thread_tid(USER_DEMO_FORK_LANE), C_CYAN);
    vga_text(17, 20, "join", C_GRAY);
    ud_put_dec(22, 20, user_demo_thread_join_ret(USER_DEMO_FORK_LANE), C_WHITE);
    vga_text(26, 20, "shared", C_GRAY);
    ud_put_dec(33, 20, user_demo_thread_shared(USER_DEMO_FORK_LANE), C_WHITE);
    vga_text(40, 20, "ok", C_GRAY);
    vga_text(43, 20,
             user_demo_thread_ok(USER_DEMO_FORK_LANE) == 0x54485244u ? "THRD" : "BAD",
             user_demo_thread_ok(USER_DEMO_FORK_LANE) == 0x54485244u ? C_GREEN : C_MAG);
    vga_text(50, 20, "det", C_GRAY);
    vga_text(54, 20,
             user_demo_thread_det_ok(USER_DEMO_FORK_LANE) == 0x44544544u ? "OK" : "..",
             user_demo_thread_det_ok(USER_DEMO_FORK_LANE) == 0x44544544u ? C_GREEN : C_YELLOW);
    vga_text(58, 20, "main", C_GRAY);
    vga_text(63, 20,
             user_demo_main_first_ok(USER_DEMO_FORK_LANE) == 0x4D544852u ? "OK" : "..",
             user_demo_main_first_ok(USER_DEMO_FORK_LANE) == 0x4D544852u ? C_GREEN : C_YELLOW);
    vga_text(67, 20, "arg", C_GRAY);
    vga_text(71, 20,
             user_demo_thread_edge_ok(USER_DEMO_FORK_LANE) == 0x45444745u ? "OK" : "..",
             user_demo_thread_edge_ok(USER_DEMO_FORK_LANE) == 0x45444745u ? C_GREEN : C_YELLOW);
    vga_text(74, 20, "grp", C_GRAY);
    vga_text(78, 20,
             user_demo_thread_group_ok(USER_DEMO_FORK_LANE) == 0x4D475250u ? "OK" : "..",
             user_demo_thread_group_ok(USER_DEMO_FORK_LANE) == 0x4D475250u ? C_GREEN : C_YELLOW);

    wait_magic = user_demo_wait_magic(USER_DEMO_FORK_LANE);
    cow_ok = user_demo_cow_ok(USER_DEMO_FORK_LANE);
    sig_ok = user_demo_sig_ok(USER_DEMO_FORK_LANE);
    exec_ok = user_demo_exec_ok(USER_DEMO_FORK_LANE);
    pipe_ok = user_demo_pipe_ok(USER_DEMO_FORK_LANE);
    spawn_ok = user_demo_spawn_ok(USER_DEMO_FORK_LANE);
    thread_ok = user_demo_thread_ok(USER_DEMO_FORK_LANE);
    det_ok = user_demo_thread_det_ok(USER_DEMO_FORK_LANE);
    block_ok = user_demo_block_ok(USER_DEMO_FORK_LANE);
    timer_ok = user_demo_timer_ok(USER_DEMO_FORK_LANE);
    priority_ok = user_demo_priority_ok(USER_DEMO_FORK_LANE);
    brk = user_demo_brk(0);
    poke = user_demo_heap_poke(0);
    peek = user_demo_heap_peek(0);
    guard = user_demo_guard_poke(0);
    bad = user_demo_bad_brk(0);

    vga_text(2, 21, "TEST", C_GRAY);
    vga_text(7, 21, "wait", C_GRAY);
    vga_text(12, 21, wait_magic == 0x4B4F ? "OK" : "..",
             wait_magic == 0x4B4F ? C_GREEN : C_YELLOW);
    vga_text(16, 21, "cow", C_GRAY);
    vga_text(20, 21, cow_ok == 0x434F57 ? "OK" : "..",
             cow_ok == 0x434F57 ? C_GREEN : C_YELLOW);
    vga_text(24, 21, "hand", C_GRAY);
    vga_text(29, 21, sig_ok == 0x48414E44 ? "OK" : "..",
             sig_ok == 0x48414E44 ? C_GREEN : C_YELLOW);
    vga_text(33, 21, "exec", C_GRAY);
    vga_text(38, 21, exec_ok == 0x45584543u ? "OK" : "..",
             exec_ok == 0x45584543u ? C_GREEN : C_YELLOW);
    vga_text(42, 21, "pipe", C_GRAY);
    vga_text(47, 21, pipe_ok == 0x45505049u ? "OK" : "..",
             pipe_ok == 0x45505049u ? C_GREEN : C_YELLOW);
    vga_text(51, 21, "spawn", C_GRAY);
    vga_text(57, 21, spawn_ok == 0x5350574Eu ? "OK" : "..",
             spawn_ok == 0x5350574Eu ? C_GREEN : C_YELLOW);
    vga_text(60, 21, "thr", C_GRAY);
    vga_text(64, 21, thread_ok == 0x54485244u ? "OK" : "..",
             thread_ok == 0x54485244u ? C_GREEN : C_YELLOW);
    vga_text(67, 21, "det", C_GRAY);
    vga_text(71, 21, det_ok == 0x44544544u ? "OK" : "..",
             det_ok == 0x44544544u ? C_GREEN : C_YELLOW);
    vga_text(74, 21, "blk", C_GRAY);
    vga_text(78, 21,
             (block_ok == 0x57424C4Bu && wait_blocks && wait_wakes &&
              join_blocks && join_wakes) ? "OK" : "..",
             (block_ok == 0x57424C4Bu && wait_blocks && wait_wakes &&
              join_blocks && join_wakes) ? C_GREEN : C_YELLOW);

    vga_text(2, 22, "A brk", C_GRAY);
    ud_put_hex(8, 22, brk, C_YELLOW);
    vga_text(17, 22, "poke", C_GRAY);
    ud_put_dec(22, 22, poke, poke ? C_GREEN : C_YELLOW);
    vga_text(25, 22, "peek", C_GRAY);
    ud_put_hex(30, 22, peek, peek == 0x48454C50u ? C_GREEN : C_YELLOW);
    vga_text(39, 22, "guard", C_GRAY);
    ud_put_dec(45, 22, guard, guard == 0 ? C_GREEN : C_MAG);
    vga_text(47, 22, "bad", C_GRAY);
    ud_put_hex(51, 22, bad, bad == 0xFFFFFFFFu ? C_GREEN : C_MAG);
    vga_text(60, 22, "tmr", C_GRAY);
    vga_text(64, 22,
             timer_ok == 0x544D5231u ? "OK" : "..",
             timer_ok == 0x544D5231u ? C_GREEN : C_YELLOW);
    vga_text(68, 22, "pri", C_GRAY);
    vga_text(72, 22,
             priority_ok == 0x50524931u ? "OK" : "..",
             priority_ok == 0x50524931u ? C_GREEN : C_YELLOW);
}
