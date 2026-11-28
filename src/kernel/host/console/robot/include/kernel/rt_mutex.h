#ifndef AEVUM_KERNEL_RT_MUTEX_H
#define AEVUM_KERNEL_RT_MUTEX_H

#include "kernel/task.h"

#define RT_MUTEX_NO_CEILING (-1)

/*
 * P13.5/P13.6：最小 RT 互斥锁。pi=1 时，等待者把 owner 的优先级提升到
 * 等待者中的最高值；ceiling=0..3 时，owner 在拿锁当下就提升到该天花板。
 * 两种模式都只支持单层持锁，owner 解锁后恢复 base_priority；不提供递归锁、
 * 多锁嵌套或用户态 ABI。
 */
struct rt_mutex {
    struct task_struct *owner;
    struct wait_queue waiters;
    u32 lock_count;
    u32 wait_count;
    u32 boost_count;
    u32 restore_count;
    int pi;                 /* 1 启用等待者优先级继承，0 关闭 */
    int ceiling;            /* P13.6：0..3 为立即天花板，-1 关闭 */
};

void rt_mutex_init(struct rt_mutex *m, int pi, int ceiling);
int rt_mutex_lock(struct rt_mutex *m);
int rt_mutex_unlock(struct rt_mutex *m);
void rt_mutex_stats(struct rt_mutex *m, u32 *locks, u32 *waits,
                    u32 *boosts, u32 *restores);

#endif
