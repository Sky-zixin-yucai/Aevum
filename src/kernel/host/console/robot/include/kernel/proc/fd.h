#ifndef AEVUM_KERNEL_PROC_FD_H
#define AEVUM_KERNEL_PROC_FD_H

#include "kernel/types.h"

struct address_space;
struct pipe;

/*
 * P4.4 每进程 fd 表的唯一 owner。fd 0/1 是 console 字符端点，pipe 从 2 起分配；
 * 表内 pipe 端点带引用，console 端点只记录 kind，close 仅关闭本进程句柄。
 */
#define PROC_FD_MAX   8
#define PROC_FD_FIRST 2

enum proc_fd_kind {
    PROC_FD_CLOSED = 0,
    PROC_FD_PIPE_READ = 1,
    PROC_FD_PIPE_WRITE = 2,
    PROC_FD_CONSOLE_READ = 3,
    PROC_FD_CONSOLE_WRITE = 4
};

struct proc_fd_entry {
    struct pipe *pipe;
    u32 kind;
};

struct proc_fd_table {
    struct proc_fd_entry entries[PROC_FD_MAX];
};

/* 只读快照：V 页观察 fd 槽，不暴露 pipe 对象指针。used=0 表示关闭/越界。 */
struct proc_fd_info {
    int used;
    u32 kind;
};

void proc_fd_table_init(struct proc_fd_table *table);
void proc_fd_table_close_all(struct proc_fd_table *table);

/* 新用户进程安装 fd 0 读 console、fd 1 写 console；free slot 保持全关。 */
void proc_fd_console_open(struct proc_fd_table *table);

/* fork：复制引用；pipe acquire，console 直接继承。exec 不调用本函数。 */
int proc_fd_table_copy(struct proc_fd_table *dst,
                       const struct proc_fd_table *src);
u32 proc_fd_count(const struct proc_fd_table *table);
int proc_fd_snapshot(const struct proc_fd_table *table, int fd,
                     struct proc_fd_info *out);

/* 系统调用语义：pipe 写两个 u32 fd；read/write 返回字节数或 -1。 */
int proc_fd_pipe(struct proc_fd_table *table, struct address_space *space,
                 u32 user_fds);
int proc_fd_read(struct proc_fd_table *table, struct address_space *space,
                 u32 fd, u32 user_buf, u32 len);
int proc_fd_write(struct proc_fd_table *table, struct address_space *space,
                  u32 fd, u32 user_buf, u32 len);
/* P11.3：带 PIT tick 上限的阻塞读写；ticks=0 等价原非阻塞语义。 */
int proc_fd_read_block(struct proc_fd_table *table, struct address_space *space,
                       u32 fd, u32 user_buf, u32 len, u32 ticks);
int proc_fd_write_block(struct proc_fd_table *table, struct address_space *space,
                        u32 fd, u32 user_buf, u32 len, u32 ticks);
int proc_fd_close(struct proc_fd_table *table, u32 fd);

#endif
