#ifndef AEVUM_KERNEL_FS_RAMFS_H
#define AEVUM_KERNEL_FS_RAMFS_H

#include "kernel/types.h"

/*
 * P12.1 只读 ramdisk。镜像由 kernel/fs/ramfs/ramdisk.asm 链接到 .rodata：
 * 16B 头 + N×24B 目录项 + 文件区，名字最多 15 字符且必须有 NUL。
 * 只支持单层根目录文件名，不支持 '/'、'.'、'..'、目录、写或创建。
 */
#define RAMFS_NAME_MAX  16
#define RAMFS_MAX_FILES 4

struct ramfs_file {
    const char *name;
    const u8 *data;
    u32 size;
};

/* 校验镜像头/目录项；成功返回 0，镜像损坏或 count 超过上限返回 -1。 */
int ramfs_init(void);

/* 精确匹配单层文件名；未初始化或不存在返回 0。 */
const struct ramfs_file *ramfs_lookup(const char *name);

/* 从文件 offset 复制最多 len 字节；返回实际字节数，offset 越界/参数错返回 -1。 */
int ramfs_read(const struct ramfs_file *file, u32 offset, u8 *buf, u32 len);

u32 ramfs_file_count(void);
u32 ramfs_image_size(void);

#endif
