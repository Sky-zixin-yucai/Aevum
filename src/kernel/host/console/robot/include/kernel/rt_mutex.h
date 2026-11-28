#ifndef AEVUM_KERNEL_RT_MUTEX_H
#define AEVUM_KERNEL_RT_MUTEX_H

#include "kernel/task.h"

/*
 * P13.5：最小 RT 互斥锁。owner 持锁时，等待者把 owner 的优先级提升到
 * 等待者中的最高值；owner 解锁后恢复 base_priority。只支持单层持锁，
 * 不提供递归锁、优先级天花板或用户态 ABI。
 */
struct rt_mutex {
    struct task_struct *owner;
    struct wait_queue waiters;
    u32 lock_count;
    u32 wait_count;
    u32 boost_count;
    u32 restore_count;
    int pi;                 /* 1 启用优先级继承，0 只做互斥对照 */
};

void rt_mutex_init(struct rt_mutex *m, int pi);
int rt_mutex_lock(struct rt_mutex *m);
int rt_mutex_unlock(struct rt_mutex *m);
void rt_mutex_stats(struct rt_mutex *m, u32 *locks, u32 *waits,
                    u32 *boosts, u32 *restores);

#endif
