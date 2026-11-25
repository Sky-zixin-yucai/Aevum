#include "kernel/fs/ramfs.h"

extern u8 ramdisk_start[];
extern u8 ramdisk_end[];

#define RAMFS_HEADER_SIZE 16u
#define RAMFS_ENTRY_SIZE  24u

static struct ramfs_file ramfs_files[RAMFS_MAX_FILES];
static u32 ramfs_count;
static u32 ramfs_size;
static int ramfs_ready;

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static int name_has_nul(const char *name)
{
    int i;

    for (i = 0; i < RAMFS_NAME_MAX; i++)
        if (name[i] == 0) return i > 0;
    return 0;
}

int ramfs_init(void)
{
    const u8 *base = (const u8 *)ramdisk_start;
    u32 total = (u32)((u32)ramdisk_end - (u32)ramdisk_start);
    u32 magic, version, count, i;
    u32 data_start;

    ramfs_ready = 0;
    ramfs_count = 0;
    ramfs_size = total;
    if (!total || total < RAMFS_HEADER_SIZE) return -1;

    magic = rd32(base);
    version = rd32(base + 4);
    count = rd32(base + 8);
    if (magic != 0x52564541u || version != 1u) return -1;
    if (!count || count > RAMFS_MAX_FILES) return -1;
    data_start = RAMFS_HEADER_SIZE + count * RAMFS_ENTRY_SIZE;
    if (data_start > total) return -1;

    for (i = 0; i < count; i++) {
        const u8 *entry = base + RAMFS_HEADER_SIZE + i * RAMFS_ENTRY_SIZE;
        const char *name = (const char *)entry;
        u32 off = rd32(entry + 16);
        u32 size = rd32(entry + 20);

        if (!name_has_nul(name)) return -1;
        if (off > total || size > total - off) return -1;
        if (size && off < data_start) return -1;
        ramfs_files[i].name = name;
        ramfs_files[i].data = base + off;
        ramfs_files[i].size = size;
    }
    ramfs_count = count;
    ramfs_ready = 1;
    return 0;
}

const struct ramfs_file *ramfs_lookup(const char *name)
{
    u32 i;
    int j;

    if (!name || !ramfs_ready) return 0;
    for (i = 0; i < ramfs_count; i++) {
        const char *a = ramfs_files[i].name;
        for (j = 0; j < RAMFS_NAME_MAX; j++) {
            if (a[j] != name[j]) break;
            if (a[j] == 0) return &ramfs_files[i];
        }
    }
    return 0;
}

int ramfs_read(const struct ramfs_file *file, u32 offset, u8 *buf, u32 len)
{
    u32 i;

    if (!file || !buf) return -1;
    if (offset > file->size) return -1;
    if (len > file->size - offset) len = file->size - offset;
    for (i = 0; i < len; i++) buf[i] = file->data[offset + i];
    return (int)len;
}

u32 ramfs_file_count(void)
{
    return ramfs_count;
}

u32 ramfs_image_size(void)
{
    return ramfs_size;
}
