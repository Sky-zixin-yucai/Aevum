#ifndef AEVUM_APP_USER_DEMO_H
#define AEVUM_APP_USER_DEMO_H

#include "kernel/types.h"

struct address_space;

#define USER_DEMO_LANES      5
#define USER_DEMO_FAULT_LANE 2
#define USER_DEMO_FORK_LANE  3
#define USER_DEMO_GUARD_LANE 4

/*
 * 五个 ring3 用户进程共用同一段虚拟代码。
 * userA/userB 正常计数并做 brk/heap peek/poke；userC 计数后探测内核页，
 * 验证 #PF 故障隔离；userD 先 fork 出 userE 验证正常 exit(7)+COW，
 * 再 fork 第二个 E 触发 #DE，验证 wait 的 SIGFPE 状态；最后 fork 第三个 E
 * 执行 exec(1)，验证 PID 不变、镜像切换和 wait(10)；再建 pipe 并 fork 第四个 E
 * 执行 exec(2)，验证 pipe 写端跨 exec 后父进程读回；userF 写 guard 页。
 */
const char *user_demo_start(void);
void user_demo_stop(void);
int user_demo_alive(void);
const char *user_demo_error(void);
int user_demo_pid(int lane);
struct address_space *user_demo_space(int lane);
int user_demo_lane_count(void);

/* userD 的数据页是 fork/wait 的证据：child pid、wait pid/status/ret、WAIT 魔数。 */
u32 user_demo_fork_child(int lane);
u32 user_demo_wait_pid(int lane);
u32 user_demo_wait_status(int lane);
u32 user_demo_wait_ret(int lane);
u32 user_demo_wait_magic(int lane);

/* COW 证据：fork 前父写 PAPA，子写 CHLD；父 wait 后读回 PAPA 并写 COW 验收字。 */
u32 user_demo_cow_readback(int lane);
u32 user_demo_cow_ok(int lane);

/* P3.2：D 的第二个子进程安装 SIGFPE handler，sigreturn 后写 HAND，
 * 再执行 in al,0x60 触发 #GP->SIGSEGV，父 wait 得到 0x8000000B。 */
u32 user_demo_sig_child(int lane);
u32 user_demo_sig_status(int lane);
u32 user_demo_sig_ret(int lane);
u32 user_demo_sig_ok(int lane);

/* P4.1：D 的第三个子进程 exec(1)，新镜像 exit(10)，父 wait 后写 EXEC。 */
u32 user_demo_exec_child(int lane);
u32 user_demo_exec_status(int lane);
u32 user_demo_exec_ret(int lane);
u32 user_demo_exec_ok(int lane);

/* P4.3：父建 pipe，子进程 exec(2) 后写 PIPE，父读回并在关闭后写 PIPE。 */
u32 user_demo_pipe_child(int lane);
u32 user_demo_pipe_status(int lane);
u32 user_demo_pipe_wait_ret(int lane);
u32 user_demo_pipe_read_fd(int lane);
u32 user_demo_pipe_write_fd(int lane);
u32 user_demo_pipe_read_ret(int lane);
u32 user_demo_pipe_ok(int lane);

/* P7.0：D 直接 spawn image 1，不经过 fork；子进程 exit(10)，父 wait 后写 SPWN。 */
u32 user_demo_spawn_child(int lane);
u32 user_demo_spawn_status(int lane);
u32 user_demo_spawn_wait_ret(int lane);
u32 user_demo_spawn_ok(int lane);

/* P7.1：D clone 一个共享 AS 的线程，写共享计数器后 thread_exit，主线程 join。 */
u32 user_demo_thread_tid(int lane);
u32 user_demo_thread_join_ret(int lane);
u32 user_demo_thread_shared(int lane);
u32 user_demo_thread_ok(int lane);

/* P7.2：第二个 worker 自 detach，主线程 join 必须失败；退出任务由调度器自动回收。 */
u32 user_demo_thread_det_tid(int lane);
u32 user_demo_thread_det_ret(int lane);
u32 user_demo_thread_det_join_ret(int lane);
u32 user_demo_thread_det_ok(int lane);

/* P7.3：非法参数矩阵，以及主线程先退、worker 继续并 join 主线程。 */
u32 user_demo_thread_edge_ok(int lane);
u32 user_demo_main_first_ok(int lane);

/* P7.4：主线程不 join，两个 worker 继续运行；最后 worker 触发整组回收。 */
u32 user_demo_thread_group_ok(int lane);

/* P7.5：父 wait 与子线程 join 都已真实进入 BLOCKED 并被唤醒。 */
u32 user_demo_block_ok(int lane);
/* P11.1：D lane 的 sleep / wait_timeout 验收字，0x401250 = "TMR1"。 */
u32 user_demo_timer_ok(int lane);
/* P11.2：D lane 的 get/setpriority + fork/clone 继承验收字，0x401260 = "PRI1"。 */
u32 user_demo_priority_ok(int lane);
/* P11.3：D lane spawn image 3 完成阻塞 I/O 验收，0x4012A0 = "IOB1"。 */
u32 user_demo_io_ok(int lane);

/* userA 的数据页是 brk/heap/guard 的证据。 */
u32 user_demo_brk(int lane);
u32 user_demo_heap_peek(int lane);
u32 user_demo_heap_poke(int lane);
u32 user_demo_guard_poke(int lane);
u32 user_demo_bad_brk(int lane);

#endif
