#ifndef AEVUM_DRIVERS_SERIAL_UART_H
#define AEVUM_DRIVERS_SERIAL_UART_H

#include "kernel/types.h"
#include "app/body/body.h"
#include "app/path/path.h"

#define UART_COM1          0x3F8
#define UART_FRAME_MAX_CELLS 32

#define UART_CMD_NONE     0
#define UART_CMD_JOINT    1
#define UART_CMD_BODY     2
#define UART_CMD_RELEASE  3
#define UART_CMD_FULL     4
#define UART_CMD_PATH     5

struct uart_input {
    int type;
    int n;
    i32 q[AEVUM_BODY_MAX_JOINTS];
    struct body_desc body;
    int path_kind;
    i32 path_raw[8];
};

/* COM1 的唯一实现入口：初始化、发送、收 B/J/R/F 命令、扫描 VGA 差量帧。 */
void uart_init(void);
void uart_write(const u8 *buf, u32 len);
int uart_poll_input(struct uart_input *out);
void uart_scan(void);
/* 完整状态帧：任意个 i32/Q16.16 值 + XOR CRC；由 app 层组装字段。 */
void uart_status_frame(const i32 *values, u32 count);
/* 通用几何路径状态帧 0x05；字段语义由 app 层决定，不是车辆专用帧。 */
void uart_path_frame(const i32 *values, u32 count);

#endif
