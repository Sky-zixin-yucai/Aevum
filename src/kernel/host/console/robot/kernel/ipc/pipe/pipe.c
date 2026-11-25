#include "kernel/ipc/pipe.h"
#include "kernel/task.h"

struct pipe {
    u32 head;
    u32 tail;
    u32 count;
    u32 refs;
    u32 readers;
    u32 writers;
    u32 used;
    struct wait_queue read_wq;
    struct wait_queue write_wq;
    u8 buf[PIPE_CAPACITY];
};

static struct pipe pipes[PIPE_MAX];

struct pipe *pipe_create(void)
{
    int i;

    for (i = 0; i < PIPE_MAX; i++) {
        struct pipe *p = &pipes[i];
        if (p->used) continue;
        p->head = 0;
        p->tail = 0;
        p->count = 0;
        p->refs = 0;
        p->readers = 0;
        p->writers = 0;
        p->used = 1;
        wait_queue_init(&p->read_wq);
        wait_queue_init(&p->write_wq);
        return p;
    }
    return 0;
}

/* 新建对象 refs=0；fd 表每装入一个端点 acquire 一次。 */
void pipe_acquire(struct pipe *p, int write_end)
{
    if (!p || !p->used) return;
    p->refs++;
    if (write_end) p->writers++;
    else p->readers++;
}

void pipe_release(struct pipe *p, int write_end)
{
    if (!p || !p->used) return;
    if (write_end) {
        if (p->writers) p->writers--;
    } else {
        if (p->readers) p->readers--;
    }
    if (p->refs) p->refs--;
    if (!p->refs) {
        wait_queue_wake_all(&p->read_wq);
        wait_queue_wake_all(&p->write_wq);
        p->used = 0;
        p->head = 0;
        p->tail = 0;
        p->count = 0;
        p->readers = 0;
        p->writers = 0;
    } else {
        if (write_end && !p->writers) wait_queue_wake_all(&p->read_wq);
        if (!write_end && !p->readers) wait_queue_wake_all(&p->write_wq);
    }
}

int pipe_read(struct pipe *p, void *buf, u32 len)
{
    u8 *out = (u8 *)buf;
    u32 i;
    u32 n;

    if (!p || !p->used || !buf) return -1;
    if (!len) return 0;
    if (!p->count)
        return p->writers ? -1 : 0;

    n = len < p->count ? len : p->count;
    for (i = 0; i < n; i++) {
        out[i] = p->buf[p->tail];
        p->tail = (p->tail + 1u) % PIPE_CAPACITY;
    }
    p->count -= n;
    /* P11.6：腾出空间只唤醒一个写者；若仍有数据，再把下一个读者交出去。 */
    wait_queue_wake_one(&p->write_wq);
    if (p->count) wait_queue_wake_one(&p->read_wq);
    return (int)n;
}

int pipe_write(struct pipe *p, const void *buf, u32 len)
{
    const u8 *in = (const u8 *)buf;
    u32 i;

    if (!p || !p->used || !buf) return -1;
    if (!len) return 0;
    if (!p->readers) return -1;
    if (len > PIPE_CAPACITY - p->count) return -1;

    for (i = 0; i < len; i++) {
        p->buf[p->head] = in[i];
        p->head = (p->head + 1u) % PIPE_CAPACITY;
    }
    p->count += len;
    /* P11.6：新数据只交给队首读者，避免广播后所有读者一起抢同一份数据。 */
    wait_queue_wake_one(&p->read_wq);
    return (int)len;
}

static int pipe_read_ready(void *ctx)
{
    struct pipe *p = (struct pipe *)ctx;
    return !p || !p->used || p->count || !p->writers;
}

struct pipe_write_wait {
    struct pipe *p;
    u32 len;
};

static int pipe_write_ready(void *ctx)
{
    struct pipe_write_wait *w = (struct pipe_write_wait *)ctx;
    struct pipe *p = w->p;
    return !p || !p->used || !p->readers ||
           w->len <= PIPE_CAPACITY - p->count;
}

int pipe_read_block(struct pipe *p, void *buf, u32 len, u32 ticks)
{
    int rc;

    if (!p || !p->used || !buf) return -1;
    if (!len) return 0;
    if (!ticks) return pipe_read(p, buf, len);
    rc = task_block_on(&p->read_wq, pipe_read_ready, p, ticks);
    if (rc == 2) return -3;     /* P11.7：pending signal -> EINTR */
    if (rc < 0) return -1;
    if (rc > 0) return -2;
    return pipe_read(p, buf, len);
}

int pipe_write_block(struct pipe *p, const void *buf, u32 len, u32 ticks)
{
    struct pipe_write_wait wait;
    int rc;

    if (!p || !p->used || !buf) return -1;
    if (!len) return 0;
    if (!p->readers || len > PIPE_CAPACITY) return -1;
    if (!ticks) return pipe_write(p, buf, len);
    wait.p = p;
    wait.len = len;
    rc = task_block_on(&p->write_wq, pipe_write_ready, &wait, ticks);
    if (rc == 2) return -3;     /* P11.7：pending signal -> EINTR */
    if (rc < 0) return -1;
    if (rc > 0) return -2;
    return pipe_write(p, buf, len);
}

u32 pipe_live_count(void)
{
    int i;
    u32 n = 0;

    for (i = 0; i < PIPE_MAX; i++)
        if (pipes[i].used) n++;
    return n;
}
