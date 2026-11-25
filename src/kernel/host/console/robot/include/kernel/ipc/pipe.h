#ifndef AEVUM_KERNEL_IPC_PIPE_H
#define AEVUM_KERNEL_IPC_PIPE_H

#include "kernel/types.h"

/*
 * P4.3 匿名 pipe 的唯一 owner。对象池和字节环形缓冲都在本模块，
 * fd 表只保存端点引用；两端 release 后对象立即回到池中。
 */
#define PIPE_MAX      4
#define PIPE_CAPACITY 64

struct pipe;

struct pipe *pipe_create(void);
void pipe_acquire(struct pipe *p, int write_end);
void pipe_release(struct pipe *p, int write_end);

/* 非阻塞：空读且仍有写端返回 -1，写端已全关返回 0；满写返回 -1。 */
int pipe_read(struct pipe *p, void *buf, u32 len);
int pipe_write(struct pipe *p, const void *buf, u32 len);
/*
 * P11.3：对象等待队列版本。ticks>0 时最多等该 PIT tick 数；
 * 返回字节数/0 EOF/-1 参数或端点错误/-2 超时。ticks=0 等价非阻塞。
 */
int pipe_read_block(struct pipe *p, void *buf, u32 len, u32 ticks);
int pipe_write_block(struct pipe *p, const void *buf, u32 len, u32 ticks);

/* 仍被 fd 引用的 pipe 对象数；用于验收关闭后无泄漏。 */
u32 pipe_live_count(void);

#endif
