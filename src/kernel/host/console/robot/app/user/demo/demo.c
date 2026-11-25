#include "app/user/demo.h"
#include "kernel/task.h"
#include "kernel/proc.h"
#include "kernel/mm/address_space.h"
#include "arch/x86/page_fault.h"
#include "arch/x86/exception.h"
#include "config/memory.h"

extern u8 user_program_start[];
extern u8 user_program_end[];

static struct process *procs[USER_DEMO_LANES];
static struct address_space *spaces[USER_DEMO_LANES];
static int pids[USER_DEMO_LANES];
static const char *volatile demo_error;

static const char *const lane_names[USER_DEMO_LANES] = {
    "userA", "userB", "userC", "userD", "userF"
};

static int lane_alive(int lane)
{
    return pids[lane] && process_alive(pids[lane]) &&
           address_space_user_active(spaces[lane]);
}

const char *user_demo_start(void)
{
    u32 code_size;
    int i;

    for (i = 0; i < USER_DEMO_LANES; i++)
        if (lane_alive(i)) return 0;

    code_size = (u32)user_program_end - (u32)user_program_start;
    demo_error = 0;
    process_cleanup();
    /* 新演示从干净的 #PF/异常记录开始；旧记录的 PID 可能被下一次复用。 */
    page_fault_clear();
    exception_clear();

    for (i = 0; i < USER_DEMO_LANES; i++) {
        procs[i] = process_create_user(1, lane_names[i], user_program_start,
                                       code_size, AEVUM_USER_STACK_TOP);
        if (!procs[i]) {
            demo_error = "process_create_user";
            user_demo_stop();
            return demo_error;
        }
        spaces[i] = process_space(procs[i]);
        pids[i] = process_pid_of(procs[i]);
        /* 同一段用户代码，用数据页里的 mode 区分正常/故障/fork-wait/guard。 */
        address_space_set_mode(spaces[i],
                               i == USER_DEMO_FAULT_LANE ? 1u :
                               (i == USER_DEMO_FORK_LANE ? 2u :
                                (i == USER_DEMO_GUARD_LANE ? 3u : 0u)));
        /* 新 lane 已 READY：让出一次，避免连续建 5 条 lane 把 render 长时间挡在创建路径。 */
        if (task_yield()) {
            demo_error = "task_yield";
            user_demo_stop();
            return demo_error;
        }
    }
    return 0;
}

void user_demo_stop(void)
{
    process_stop_all();
}

int user_demo_alive(void)
{
    int i;
    for (i = 0; i < USER_DEMO_LANES; i++)
        if (lane_alive(i)) return 1;
    return 0;
}

const char *user_demo_error(void)
{
    return demo_error;
}

int user_demo_pid(int lane)
{
    return (lane >= 0 && lane < USER_DEMO_LANES) ? pids[lane] : 0;
}

struct address_space *user_demo_space(int lane)
{
    return (lane >= 0 && lane < USER_DEMO_LANES) ? spaces[lane] : 0;
}

int user_demo_lane_count(void)
{
    return USER_DEMO_LANES;
}

u32 user_demo_fork_child(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x0Cu);
}

u32 user_demo_wait_pid(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x14u);
}

u32 user_demo_wait_status(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x10u);
}

u32 user_demo_wait_ret(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x18u);
}

u32 user_demo_wait_magic(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x1Cu);
}

u32 user_demo_cow_readback(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x3Cu);
}

u32 user_demo_cow_ok(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x40u);
}

u32 user_demo_sig_child(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x44u);
}

u32 user_demo_sig_status(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x48u);
}

u32 user_demo_sig_ret(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x50u);
}

u32 user_demo_sig_ok(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x4Cu);
}

u32 user_demo_exec_child(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x58u);
}

u32 user_demo_exec_status(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x5Cu);
}

u32 user_demo_exec_ret(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x64u);
}

u32 user_demo_exec_ok(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x60u);
}

u32 user_demo_pipe_child(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x80u);
}

u32 user_demo_pipe_status(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x84u);
}

u32 user_demo_pipe_wait_ret(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x88u);
}

u32 user_demo_pipe_read_fd(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x6Cu);
}

u32 user_demo_pipe_write_fd(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x70u);
}

u32 user_demo_pipe_read_ret(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x98u);
}

u32 user_demo_pipe_ok(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x7Cu);
}

u32 user_demo_spawn_child(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x144u);
}

u32 user_demo_spawn_status(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x148u);
}

u32 user_demo_spawn_wait_ret(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x14Cu);
}

u32 user_demo_spawn_ok(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x150u);
}

u32 user_demo_thread_tid(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x158u);
}

u32 user_demo_thread_join_ret(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x16Cu);
}

u32 user_demo_thread_shared(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x160u);
}

u32 user_demo_thread_ok(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x170u);
}

u32 user_demo_thread_det_tid(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x174u);
}

u32 user_demo_thread_det_ret(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x17Cu);
}

u32 user_demo_thread_det_join_ret(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x188u);
}

u32 user_demo_thread_det_ok(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x18Cu);
}

u32 user_demo_thread_edge_ok(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x1BCu);
}

u32 user_demo_main_first_ok(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x1C4u);
}

u32 user_demo_thread_group_ok(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x1F8u);
}

u32 user_demo_block_ok(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x22Cu);
}

u32 user_demo_timer_ok(int lane)
{
    if (lane != USER_DEMO_FORK_LANE || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x250u);
}

u32 user_demo_brk(int lane)
{
    if (lane < 0 || lane >= USER_DEMO_LANES || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x20u);
}

u32 user_demo_heap_peek(int lane)
{
    if (lane < 0 || lane >= USER_DEMO_LANES || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x24u);
}

u32 user_demo_heap_poke(int lane)
{
    if (lane < 0 || lane >= USER_DEMO_LANES || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x28u);
}

u32 user_demo_guard_poke(int lane)
{
    if (lane < 0 || lane >= USER_DEMO_LANES || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x2Cu);
}

u32 user_demo_bad_brk(int lane)
{
    if (lane < 0 || lane >= USER_DEMO_LANES || !spaces[lane]) return 0;
    return address_space_user_read32(spaces[lane], AEVUM_USER_DATA_VA + 0x34u);
}
