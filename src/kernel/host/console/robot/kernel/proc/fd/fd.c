#include "kernel/proc/fd.h"
#include "kernel/fs/ramfs.h"
#include "kernel/ipc/pipe.h"
#include "kernel/mm/address_space.h"
#include "drivers/console/console.h"

static int proc_fd_is_pipe(u32 kind)
{
    return kind == PROC_FD_PIPE_READ || kind == PROC_FD_PIPE_WRITE;
}

static void proc_fd_entry_close(struct proc_fd_entry *e)
{
    if (!e || e->kind == PROC_FD_CLOSED) return;
    if (proc_fd_is_pipe(e->kind))
        pipe_release(e->pipe, e->kind == PROC_FD_PIPE_WRITE);
    e->pipe = 0;
    e->file = 0;
    e->offset = 0;
    e->kind = PROC_FD_CLOSED;
}

static void proc_fd_table_link(struct proc_fd_table *table, u32 fd,
                               struct pipe *p, u32 kind)
{
    table->entries[fd].pipe = p;
    table->entries[fd].offset = 0;
    table->entries[fd].kind = kind;
    if (proc_fd_is_pipe(kind)) pipe_acquire(p, kind == PROC_FD_PIPE_WRITE);
}

void proc_fd_table_init(struct proc_fd_table *table)
{
    int i;

    if (!table) return;
    for (i = 0; i < PROC_FD_MAX; i++) {
        table->entries[i].pipe = 0;
        table->entries[i].file = 0;
        table->entries[i].offset = 0;
        table->entries[i].kind = PROC_FD_CLOSED;
    }
}

void proc_fd_table_close_all(struct proc_fd_table *table)
{
    int i;

    if (!table) return;
    for (i = 0; i < PROC_FD_MAX; i++)
        proc_fd_entry_close(&table->entries[i]);
}

void proc_fd_console_open(struct proc_fd_table *table)
{
    if (!table) return;
    proc_fd_table_link(table, 0, 0, PROC_FD_CONSOLE_READ);
    proc_fd_table_link(table, 1, 0, PROC_FD_CONSOLE_WRITE);
}

int proc_fd_table_copy(struct proc_fd_table *dst,
                       const struct proc_fd_table *src)
{
    int i;

    if (!dst || !src) return -1;
    proc_fd_table_init(dst);
    for (i = 0; i < PROC_FD_MAX; i++) {
        if (src->entries[i].kind == PROC_FD_CLOSED) continue;
        if (src->entries[i].kind == PROC_FD_RAMFS_FILE) {
            dst->entries[i].pipe = 0;
            dst->entries[i].file = src->entries[i].file;
            dst->entries[i].offset = src->entries[i].offset;
            dst->entries[i].kind = PROC_FD_RAMFS_FILE;
        } else {
            proc_fd_table_link(dst, (u32)i, src->entries[i].pipe,
                               src->entries[i].kind);
        }
    }
    return 0;
}

u32 proc_fd_count(const struct proc_fd_table *table)
{
    int i;
    u32 n = 0;

    if (!table) return 0;
    for (i = 0; i < PROC_FD_MAX; i++)
        if (table->entries[i].kind != PROC_FD_CLOSED) n++;
    return n;
}

int proc_fd_open_ramfs(struct proc_fd_table *table, struct address_space *space,
                       u32 user_path, u32 flags)
{
    char path[RAMFS_NAME_MAX];
    const struct ramfs_file *file;
    u32 fd;
    int i;
    int done = 0;

    if (!table || !space || !user_path) return -1;
    if (flags != 0) return -1;

    for (i = 0; i < RAMFS_NAME_MAX; i++) {
        u8 c;
        if (address_space_copyin(space, &c, user_path + (u32)i, 1) != 0)
            return -1;
        path[i] = (char)c;
        if (c == 0) {
            if (i == 0) return -1;
            done = 1;
            break;
        }
    }
    if (!done) return -1;

    file = ramfs_lookup(path);
    if (!file) return -1;
    for (fd = PROC_FD_FIRST; fd < PROC_FD_MAX; fd++)
        if (table->entries[fd].kind == PROC_FD_CLOSED) break;
    if (fd == PROC_FD_MAX) return -1;

    table->entries[fd].pipe = 0;
    table->entries[fd].file = file;
    table->entries[fd].offset = 0;
    table->entries[fd].kind = PROC_FD_RAMFS_FILE;
    return (int)fd;
}

int proc_fd_snapshot(const struct proc_fd_table *table, int fd,
                     struct proc_fd_info *out)
{
    if (!table || !out || fd < 0 || fd >= PROC_FD_MAX) return 0;
    out->kind = table->entries[fd].kind;
    out->used = out->kind != PROC_FD_CLOSED;
    return out->used;
}

int proc_fd_pipe(struct proc_fd_table *table, struct address_space *space,
                 u32 user_fds)
{
    struct pipe *p;
    struct {
        u32 read_fd;
        u32 write_fd;
    } out;
    u32 r;
    u32 w;

    if (!table || !space || !user_fds) return -1;
    if (!address_space_user_range_ok(space, user_fds, 8, 1)) return -1;

    for (r = PROC_FD_FIRST; r < PROC_FD_MAX; r++)
        if (table->entries[r].kind == PROC_FD_CLOSED) break;
    if (r == PROC_FD_MAX) return -1;
    for (w = r + 1u; w < PROC_FD_MAX; w++)
        if (table->entries[w].kind == PROC_FD_CLOSED) break;
    if (w == PROC_FD_MAX) return -1;

    p = pipe_create();
    if (!p) return -1;
    proc_fd_table_link(table, r, p, PROC_FD_PIPE_READ);
    proc_fd_table_link(table, w, p, PROC_FD_PIPE_WRITE);
    out.read_fd = r;
    out.write_fd = w;
    if (address_space_copyout(space, user_fds, &out, sizeof(out)) != 0) {
        proc_fd_entry_close(&table->entries[r]);
        proc_fd_entry_close(&table->entries[w]);
        return -1;
    }
    return 0;
}

int proc_fd_read(struct proc_fd_table *table, struct address_space *space,
                 u32 fd, u32 user_buf, u32 len)
{
    struct proc_fd_entry *e;
    u8 buf[PIPE_CAPACITY];
    int n;

    if (!table || !space || fd >= PROC_FD_MAX) return -1;
    if (!len) return 0;
    if (!user_buf) return -1;
    e = &table->entries[fd];

    if (e->kind == PROC_FD_CONSOLE_READ) {
        if (len > CONSOLE_INPUT_MAX) len = CONSOLE_INPUT_MAX;
        if (!address_space_user_range_ok(space, user_buf, len, 1)) return -1;
        n = console_read(buf, len);
        if (n <= 0) return n;
        if (address_space_copyout(space, user_buf, buf, (u32)n) != 0) return -1;
        return n;
    }

    if (e->kind == PROC_FD_RAMFS_FILE) {
        if (len > PIPE_CAPACITY) len = PIPE_CAPACITY;
        if (!address_space_user_range_ok(space, user_buf, len, 1)) return -1;
        n = ramfs_read(e->file, e->offset, buf, len);
        if (n <= 0) return n;
        if (address_space_copyout(space, user_buf, buf, (u32)n) != 0) return -1;
        e->offset += (u32)n;
        return n;
    }

    if (e->kind != PROC_FD_PIPE_READ) return -1;
    if (len > PIPE_CAPACITY) len = PIPE_CAPACITY;
    if (!address_space_user_range_ok(space, user_buf, len, 1)) return -1;

    n = pipe_read(e->pipe, buf, len);
    if (n <= 0) return n;
    if (address_space_copyout(space, user_buf, buf, (u32)n) != 0) return -1;
    return n;
}

int proc_fd_write(struct proc_fd_table *table, struct address_space *space,
                  u32 fd, u32 user_buf, u32 len)
{
    struct proc_fd_entry *e;
    u8 buf[PIPE_CAPACITY];

    if (!table || !space || fd >= PROC_FD_MAX) return -1;
    if (!len) return 0;
    if (!user_buf) return -1;
    e = &table->entries[fd];

    if (e->kind == PROC_FD_CONSOLE_WRITE) {
        if (len > CONSOLE_WRITE_MAX) return -1;
        if (!address_space_user_range_ok(space, user_buf, len, 0)) return -1;
        if (address_space_copyin(space, buf, user_buf, len) != 0) return -1;
        return console_write(buf, len);
    }

    if (e->kind == PROC_FD_RAMFS_FILE) return -1;
    if (e->kind != PROC_FD_PIPE_WRITE) return -1;
    if (len > PIPE_CAPACITY) return -1;
    if (!address_space_user_range_ok(space, user_buf, len, 0)) return -1;
    if (address_space_copyin(space, buf, user_buf, len) != 0) return -1;
    return pipe_write(e->pipe, buf, len);
}

int proc_fd_close(struct proc_fd_table *table, u32 fd)
{
    if (!table || fd >= PROC_FD_MAX) return -1;
    if (table->entries[fd].kind == PROC_FD_CLOSED) return -1;
    proc_fd_entry_close(&table->entries[fd]);
    return 0;
}

int proc_fd_read_block(struct proc_fd_table *table, struct address_space *space,
                       u32 fd, u32 user_buf, u32 len, u32 ticks)
{
    struct proc_fd_entry *e;
    u8 buf[PIPE_CAPACITY];
    int n;

    if (!table || !space || fd >= PROC_FD_MAX) return -1;
    if (!len) return 0;
    if (!user_buf) return -1;
    e = &table->entries[fd];

    if (e->kind == PROC_FD_CONSOLE_READ) {
        if (len > CONSOLE_INPUT_MAX) len = CONSOLE_INPUT_MAX;
        if (!address_space_user_range_ok(space, user_buf, len, 1)) return -1;
        n = console_read_block(buf, len, ticks);
        if (n <= 0) return n;
        if (address_space_copyout(space, user_buf, buf, (u32)n) != 0) return -1;
        return n;
    }

    if (e->kind == PROC_FD_RAMFS_FILE) {
        if (len > PIPE_CAPACITY) len = PIPE_CAPACITY;
        if (!address_space_user_range_ok(space, user_buf, len, 1)) return -1;
        n = ramfs_read(e->file, e->offset, buf, len);
        if (n <= 0) return n;
        if (address_space_copyout(space, user_buf, buf, (u32)n) != 0) return -1;
        e->offset += (u32)n;
        return n;
    }

    if (e->kind != PROC_FD_PIPE_READ) return -1;
    if (len > PIPE_CAPACITY) len = PIPE_CAPACITY;
    if (!address_space_user_range_ok(space, user_buf, len, 1)) return -1;

    n = pipe_read_block(e->pipe, buf, len, ticks);
    if (n <= 0) return n;
    if (address_space_copyout(space, user_buf, buf, (u32)n) != 0) return -1;
    return n;
}

int proc_fd_write_block(struct proc_fd_table *table, struct address_space *space,
                        u32 fd, u32 user_buf, u32 len, u32 ticks)
{
    struct proc_fd_entry *e;
    u8 buf[PIPE_CAPACITY];

    if (!table || !space || fd >= PROC_FD_MAX) return -1;
    if (!len) return 0;
    if (!user_buf) return -1;
    e = &table->entries[fd];

    if (e->kind == PROC_FD_CONSOLE_WRITE) {
        if (len > CONSOLE_WRITE_MAX) return -1;
        if (!address_space_user_range_ok(space, user_buf, len, 0)) return -1;
        if (address_space_copyin(space, buf, user_buf, len) != 0) return -1;
        return console_write(buf, len);
    }

    if (e->kind == PROC_FD_RAMFS_FILE) return -1;
    if (e->kind != PROC_FD_PIPE_WRITE) return -1;
    if (len > PIPE_CAPACITY) return -1;
    if (!address_space_user_range_ok(space, user_buf, len, 0)) return -1;
    if (address_space_copyin(space, buf, user_buf, len) != 0) return -1;
    return pipe_write_block(e->pipe, buf, len, ticks);
}
