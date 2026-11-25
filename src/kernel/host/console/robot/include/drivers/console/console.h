#ifndef AEVUM_DRIVERS_CONSOLE_CONSOLE_H
#define AEVUM_DRIVERS_CONSOLE_CONSOLE_H

#include "kernel/types.h"

/*
 * P4.4 console 字符设备：fd 0 的输入源与 fd 1 的输出汇。
 * 输入只保存可打印 ASCII；输出同时进入 VGA 页面行缓存和 COM1。
 * 所有函数都由单个调度线程调用，不做并发保护。
 */
#define CONSOLE_INPUT_MAX    16
#define CONSOLE_LINES        8
#define CONSOLE_LINE_MAX     64
#define CONSOLE_WRITE_MAX    64

void console_reset(void);

/* 键盘扫描码 -> ASCII 队列；X 等页面命令键由 app 自己消费，不进入这里。 */
void console_input_push(u8 scan);
/* 非阻塞读：返回 1..len，空队列返回 -1。 */
int console_read(u8 *buf, u32 len);
/* P11.3：最多等 ticks 个 PIT tick；返回字节数/-1/-2 超时，ticks=0 非阻塞。 */
int console_read_block(u8 *buf, u32 len, u32 ticks);
u32 console_queue_count(void);
u32 console_input_dropped(void);
u32 console_scan_total(void);
u32 console_push_total(void);

/* 写 fd 1：返回写入字节数；调用方保证 len <= CONSOLE_WRITE_MAX。 */
int console_write(const u8 *buf, u32 len);

u32 console_output_total(void);
u32 console_output_bytes(void);
u32 console_uart_bytes(void);
u32 console_read_total(void);
u32 console_last_input(void);
u32 console_last_output(void);

u32 console_line_count(void);
const char *console_line(u32 index);

#endif
