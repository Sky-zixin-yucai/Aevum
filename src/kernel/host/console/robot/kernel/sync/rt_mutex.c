#include "kernel/rt_mutex.h"

static int rt_mutex_free(void *ctx)
{
    struct rt_mutex *m = (struct rt_mutex *)ctx;

    return m && !m->owner;
}

void rt_mutex_init(struct rt_mutex *m, int pi, int ceiling)
{
    if (!m) return;
    m->owner = 0;
    wait_queue_init(&m->waiters);
    m->lock_count = 0;
    m->wait_count = 0;
    m->boost_count = 0;
    m->restore_count = 0;
    m->pi = pi ? 1 : 0;
    m->ceiling = (ceiling >= 0 && ceiling <= TASK_PRIORITY_MAX)
                     ? ceiling : RT_MUTEX_NO_CEILING;
    m->chain = m->pi;
}

void rt_mutex_set_chain(struct rt_mutex *m, int chain)
{
    if (m) m->chain = chain ? 1 : 0;
}

int rt_mutex_lock(struct rt_mutex *m)
{
    struct task_struct *owner;
    struct task_struct *self;
    u32 flags;
    int rc;
    int boosts;
    int cur_priority;

    if (!m) return -1;

    for (;;) {
        __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) : : "memory");
        self = task_current();
        if (!m->owner) {
            if (task_rt_mutex_held_depth(self) >= TASK_RT_MUTEX_HOLD_MAX) {
                __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
                return -1;
            }
            m->owner = self;
            m->lock_count++;
            if (task_rt_mutex_hold(self, m) != 0) {
                m->owner = 0;
                __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
                return -1;
            }
            /*
             * P13.6：立即优先级天花板。owner 拿锁当下就提升，不等到
             * 高优先级任务来等锁才提升；base 已经高于天花板时不动。
             */
            if (m->ceiling >= 0 && m->ceiling < task_priority_get(0)) {
                if (task_rt_mutex_boost(self, m->ceiling))
                    m->boost_count++;
            }
            __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
            return 0;
        }

        owner = m->owner;
        if (owner == self) {
            __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");
            return -2;      /* 递归锁不在 P13.5/P13.6/P13.7 范围 */
        }

        /*
         * P13.7：先记录当前任务阻塞在哪把锁，再提升 owner。若 owner 自己
         * 也阻塞在内层锁上，boost_chain 会继续提升内层 owner；最多 2 层。
         */
        task_rt_mutex_set_blocked_on(self, m);
        if (m->pi) {
            cur_priority = task_priority_get(0);
            boosts = m->chain
                         ? task_rt_mutex_boost_chain(owner, cur_priority, 0)
                         : task_rt_mutex_boost(owner, cur_priority);
            m->boost_count += (u32)boosts;
        }
        __asm__ __volatile__("push %0; popf" : : "r"(flags) : "memory", "cc");

        m->wait_count++;
        rc = task_block_on(&m->waiters, rt_mutex_free, m, 0x3fffffffu);
        task_rt_mutex_set_blocked_on(self, 0);
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

    if (task_rt_mutex_unhold(owner, m) != 0) return -1;
    m->owner = 0;
    /*
     * P13.6：先恢复 owner 的基准优先级，再唤醒等待者。否则 PIT 可能
     * 在 wake_one 和 restore 之间切到高优先级等待者，统计里会看到
     * boost=1 但 restore=0，owner 也会短暂以提升后的优先级继续跑。
     * P13.7：只有最外层解锁才恢复；仍持有内层锁时保持提升。恢复不依赖
     * 当前这把锁是否 pi/ceiling，避免“内层 PI 锁 + 外层普通锁”漏恢复。
     */
    if (task_rt_mutex_held_depth(owner) == 0 &&
        task_rt_mutex_restore(owner))
        m->restore_count++;
    wait_queue_wake_one(&m->waiters);
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
