#include "kernel/rt_mutex.h"

static int rt_mutex_free(void *ctx)
{
    struct rt_mutex *m = (struct rt_mutex *)ctx;

    return m && !m->owner;
}

void rt_mutex_init(struct rt_mutex *m, int pi)
{
    if (!m) return;
    m->owner = 0;
    wait_queue_init(&m->waiters);
    m->lock_count = 0;
    m->wait_count = 0;
    m->boost_count = 0;
    m->restore_count = 0;
    m->pi = pi ? 1 : 0;
}

int rt_mutex_lock(struct rt_mutex *m)
{
    struct task_struct *owner;
    u32 flags;
    int rc;

    if (!m) return -1;

    for (;;) {
        __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
        if (!m->owner) {
            m->owner = task_current();
            m->lock_count++;
            __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
            return 0;
        }

        owner = m->owner;
        if (owner == task_current()) {
            __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
            return -2;      /* 递归锁不在 P13.5 范围 */
        }

        /*
         * 先提升 owner，再进入等待队列。owner 刚解锁的窗口由
         * task_block_on() 的 ready 回调兜住，不会丢唤醒。
         */
        if (m->pi) {
            (void)task_rt_mutex_boost(owner, task_priority_get(0));
            m->boost_count++;
        }
        __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");

        m->wait_count++;
        rc = task_block_on(&m->waiters, rt_mutex_free, m, 0x3fffffffu);
        if (rc == 2) return -3;     /* EINTR */
        if (rc < 0) return -1;
    }
}

int rt_mutex_unlock(struct rt_mutex *m)
{
    struct task_struct *owner;

    if (!m) return -1;
    owner = m->owner;
    if (!owner || owner != task_current()) return -1;

    m->owner = 0;
    wait_queue_wake_one(&m->waiters);
    if (m->pi && task_rt_mutex_restore(owner))
        m->restore_count++;
    return 0;
}

void rt_mutex_stats(struct rt_mutex *m, u32 *locks, u32 *waits,
                    u32 *boosts, u32 *restores)
{
    if (locks) *locks = m ? m->lock_count : 0;
    if (waits) *waits = m ? m->wait_count : 0;
    if (boosts) *boosts = m ? m->boost_count : 0;
    if (restores) *restores = m ? m->restore_count : 0;
}
