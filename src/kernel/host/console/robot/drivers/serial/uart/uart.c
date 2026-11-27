#include "drivers/serial/uart.h"
#include "drivers/video/vga.h"
#include "arch/x86/io.h"
#include "config/memory.h"
#include "lib/fixed.h"

#define FRAME_SYNC0      0xA5
#define FRAME_SYNC1      0x5A
#define FRAME_TYPE_DELTA 0x01
#define FRAME_TYPE_STATUS 0x03
#define FRAME_TYPE_PATH  0x05
#define FRAME_HDR        4
#define FRAME_BUF        (FRAME_HDR + 34 * 4 + 1)
#define FRAME_STATUS_MAX 34

static u16 *const serial_snapshot = (u16 *)AEVUM_FRAME_SNAP;
static u8 serial_frame[FRAME_BUF];
static u32 serial_scan_count = 0;
static volatile u32 serial_full_request = 0;
static u8 cmd_mode;   /* 0 idle, 1 joint, 2 body */
static u8 cmd_count;  /* joint count, 0 means waiting for the count byte */
static u8 cmd_need;   /* payload bytes after count */
static u8 cmd_len;
static u8 cmd_buf[AEVUM_BODY_MAX_JOINTS * 16];

void uart_init(void)
{
    port_outb(UART_COM1 + 1, 0x00);
    port_outb(UART_COM1 + 3, 0x80);
    port_outb(UART_COM1 + 0, 0x01);
    port_outb(UART_COM1 + 1, 0x00);
    port_outb(UART_COM1 + 3, 0x03);
    port_outb(UART_COM1 + 2, 0xC7);
    port_outb(UART_COM1 + 4, 0x0B);
}

static void uart_putc(u8 value)
{
    while (!(port_inb(UART_COM1 + 5) & 0x20)) { }
    port_outb(UART_COM1, value);
}

void uart_write(const u8 *buf, u32 len)
{
    u32 i;
    for (i = 0; i < len; i++) uart_putc(buf[i]);
}

static void cmd_reset(void)
{
    cmd_mode = 0;
    cmd_count = 0;
    cmd_need = 0;
    cmd_len = 0;
}

static i32 cmd_i32(const u8 *p)
{
    u32 v = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
    return (i32)v;
}

static int cmd_finish(struct uart_input *out)
{
    int i;
    if (cmd_mode == 3) {
        if (cmd_buf[0] != PATH_KIND_LINE) {
            cmd_reset();
            return 0;
        }
        out->path.x0 = cmd_i32(cmd_buf + 1);
        out->path.z0 = cmd_i32(cmd_buf + 5);
        out->path.x1 = cmd_i32(cmd_buf + 9);
        out->path.z1 = cmd_i32(cmd_buf + 13);
        out->path.speed = cmd_i32(cmd_buf + 17);
        out->path.tol = cmd_i32(cmd_buf + 21);
        out->path.timeout = cmd_i32(cmd_buf + 25);
        out->type = UART_CMD_PATH;
        cmd_reset();
        return 1;
    }
    if (cmd_mode == 2) {
        out->n = (int)cmd_count;
        out->body.n = (int)cmd_count;
        for (i = 0; i < (int)cmd_count; i++) {
            const u8 *p = cmd_buf + i * 16;
            out->body.joints[i].length = cmd_i32(p + 0);
            out->body.joints[i].q_min = cmd_i32(p + 4);
            out->body.joints[i].q_max = cmd_i32(p + 8);
            out->body.joints[i].q_home = cmd_i32(p + 12);
        }
        out->type = UART_CMD_BODY;
        cmd_reset();
        return 1;
    }
    if (cmd_mode == 1) {
        out->n = (int)cmd_count;
        for (i = 0; i < (int)cmd_count; i++) {
            i32 raw = (i32)cmd_buf[i * 2] | ((i32)cmd_buf[i * 2 + 1] << 8);
            if (raw & 0x8000) raw -= 0x10000;
            /* 0.001 rad -> Q16.16 rad；拆 65 + 536/1000 避免 64 位除法。 */
            out->q[i] = raw * 65 + (raw * 536) / 1000;
        }
        out->type = UART_CMD_JOINT;
        cmd_reset();
        return 1;
    }
    cmd_reset();
    return 0;
}

/*
 * 宿主命令：
 *   J + u8 n + n * int16 LE      关节目标，单位 0.001 rad
 *   B + u8 n + n * 4 * i32 LE    平面身体描述（length/min/max/home）
 *   P + u8 kind + 8 * i32 LE    几何路径参数（kind=1: x0/z0/x1/z1/speed/tol/timeout/reserved）
 *   R                            释放手动
 *   F                            请求全量 VGA
 * 命令字节只在空闲时识别；J/B 的载荷可能恰好是 'F'/'R'/'J'，不能误判。
 */
int uart_poll_input(struct uart_input *out)
{
    int got = 0;
    if (!out) return 0;
    out->type = UART_CMD_NONE;

    while (port_inb(UART_COM1 + 5) & 1) {
        u8 value = port_inb(UART_COM1);

        if (cmd_mode == 0) {
            if (value == 'F') {
                serial_full_request = 1;
            } else if (value == 'R') {
                out->type = UART_CMD_RELEASE;
                return 1;
            } else if (value == 'J') {
                cmd_mode = 1;
                cmd_count = 0;
                cmd_need = 0;
                cmd_len = 0;
            } else if (value == 'B') {
                cmd_mode = 2;
                cmd_count = 0;
                cmd_need = 0;
                cmd_len = 0;
            } else if (value == 'P') {
                cmd_mode = 3;
                cmd_count = 0;
                cmd_need = 33;
                cmd_len = 0;
            }
            continue;
        }

        if (cmd_mode == 3) {
            cmd_buf[cmd_len++] = value;
            if (cmd_len == cmd_need) {
                if (cmd_finish(out)) return 1;
                got = 1;
            }
            continue;
        }

        if (cmd_count == 0) {
            /* 第一个载荷字节是关节数。 */
            if (value < AEVUM_BODY_MIN_JOINTS || value > AEVUM_BODY_MAX_JOINTS) {
                cmd_reset();
                continue;
            }
            cmd_count = value;
            cmd_need = (u8)(cmd_mode == 1 ? cmd_count * 2 : cmd_count * 16);
            cmd_len = 0;
            continue;
        }

        cmd_buf[cmd_len++] = value;
        if (cmd_len == cmd_need) {
            if (cmd_finish(out)) return 1;
            got = 1;
        }
    }
    return got;
}

static void uart_send_frame(u32 count)
{
    u32 total, i;
    u8 crc = 0;

    if (!count) return;
    serial_frame[3] = (u8)count;
    total = FRAME_HDR + count * 4;
    for (i = 0; i < total; i++) crc ^= serial_frame[i];
    serial_frame[total] = crc;
    uart_write(serial_frame, total + 1);
}

void uart_scan(void)
{
    const u16 *vga = vga_physical_buffer();
    u32 count = 0;
    u32 i;
    u32 full = serial_full_request || (serial_scan_count == 0) ||
               ((serial_scan_count % 120) == 0);
    if (full) serial_full_request = 0;

    serial_frame[0] = FRAME_SYNC0;
    serial_frame[1] = FRAME_SYNC1;
    serial_frame[2] = FRAME_TYPE_DELTA;

    for (i = 0; i < VGA_CELLS; i++) {
        u16 value = vga[i];
        if (!full && value == serial_snapshot[i]) continue;

        if (count >= UART_FRAME_MAX_CELLS) {
            uart_send_frame(count);
            count = 0;
            serial_frame[0] = FRAME_SYNC0;
            serial_frame[1] = FRAME_SYNC1;
            serial_frame[2] = FRAME_TYPE_DELTA;
        }

        {
            u32 p = FRAME_HDR + count * 4;
            serial_frame[p + 0] = (u8)(i / VGA_COLS);
            serial_frame[p + 1] = (u8)(i % VGA_COLS);
            serial_frame[p + 2] = (u8)(value & 0xFF);
            serial_frame[p + 3] = (u8)((value >> 8) & 0xFF);
        }
        serial_snapshot[i] = value;
        count++;
    }
    uart_send_frame(count);
    serial_scan_count++;
}

/*
 * 完整状态帧：A5 5A TYPE COUNT + COUNT 个 i32 LE + XOR CRC。
 * count 是 4 字节字段数，不是 VGA 单元数；app 层负责字段顺序。
 */
static void uart_value_frame(u8 type, const i32 *values, u32 count)
{
    u32 i;
    u32 total;
    u8 crc = 0;

    if (count == 0 || count > FRAME_STATUS_MAX) return;
    total = FRAME_HDR + count * 4;
    serial_frame[0] = FRAME_SYNC0;
    serial_frame[1] = FRAME_SYNC1;
    serial_frame[2] = type;
    serial_frame[3] = (u8)count;
    for (i = 0; i < count; i++) {
        u32 v = (u32)values[i];
        u32 p = FRAME_HDR + i * 4;
        serial_frame[p + 0] = (u8)(v & 0xFF);
        serial_frame[p + 1] = (u8)((v >> 8) & 0xFF);
        serial_frame[p + 2] = (u8)((v >> 16) & 0xFF);
        serial_frame[p + 3] = (u8)((v >> 24) & 0xFF);
    }
    for (i = 0; i < total; i++) crc ^= serial_frame[i];
    serial_frame[total] = crc;
    uart_write(serial_frame, total + 1);
}

void uart_status_frame(const i32 *values, u32 count)
{
    uart_value_frame(FRAME_TYPE_STATUS, values, count);
}

void uart_path_frame(const i32 *values, u32 count)
{
    uart_value_frame(FRAME_TYPE_PATH, values, count);
}
