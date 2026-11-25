#include "drivers/console/console.h"
#include "drivers/serial/uart.h"
#include "kernel/task.h"

static u8 input_queue[CONSOLE_INPUT_MAX];
static u32 input_head;
static u32 input_tail;
static u32 input_count;
static u32 input_dropped;
static u32 scan_total;
static u32 push_total;
static u32 read_total;
static u32 last_input;
static u8 shift_down;
static struct wait_queue input_wq;

static char output_lines[CONSOLE_LINES][CONSOLE_LINE_MAX];
static u32 output_line_count;
static u32 output_cursor;
static u32 output_total;
static u32 output_bytes;
static u32 uart_bytes;
static u32 last_output;

static const u8 key_base[0x3A] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`', [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/', [0x39] = ' '
};

static const u8 key_shift[0x3A] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}', [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
    [0x27] = ':', [0x28] = '"', [0x29] = '~', [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?', [0x39] = ' '
};

static void console_line_clear(u32 line)
{
    u32 i;
    for (i = 0; i < CONSOLE_LINE_MAX; i++) output_lines[line][i] = 0;
}

static void console_line_next(void)
{
    u32 i;

    if (output_line_count == 0) {
        output_line_count = 1;
        output_cursor = 0;
        console_line_clear(0);
        return;
    }
    if (output_line_count < CONSOLE_LINES) {
        console_line_clear(output_line_count);
        output_line_count++;
        output_cursor = 0;
        return;
    }
    for (i = 0; i + 1 < CONSOLE_LINES; i++) {
        u32 j;
        for (j = 0; j < CONSOLE_LINE_MAX; j++)
            output_lines[i][j] = output_lines[i + 1][j];
    }
    console_line_clear(CONSOLE_LINES - 1);
    output_cursor = 0;
}

static void console_put_byte(u8 value)
{
    if (value == '\r') return;
    if (value == '\n') {
        console_line_next();
        return;
    }
    if (output_line_count == 0) {
        output_line_count = 1;
        output_cursor = 0;
        console_line_clear(0);
    }
    if (output_cursor + 1 >= CONSOLE_LINE_MAX) console_line_next();
    output_lines[output_line_count - 1][output_cursor++] = (char)value;
    output_lines[output_line_count - 1][output_cursor] = 0;
}

void console_reset(void)
{
    u32 i;

    input_head = 0;
    input_tail = 0;
    input_count = 0;
    input_dropped = 0;
    scan_total = 0;
    push_total = 0;
    read_total = 0;
    last_input = 0;
    shift_down = 0;
    wait_queue_init(&input_wq);

    output_line_count = 0;
    output_cursor = 0;
    output_total = 0;
    output_bytes = 0;
    uart_bytes = 0;
    last_output = 0;
    for (i = 0; i < CONSOLE_LINES; i++) console_line_clear(i);
}

void console_input_push(u8 scan)
{
    u8 value;

    if (!scan) return;
    scan_total++;
    if (scan == 0x2A || scan == 0x36) {
        shift_down = 1;
        return;
    }
    if (scan == 0xAA || scan == 0xB6) {
        shift_down = 0;
        return;
    }
    if (!scan || (scan & 0x80) || scan >= 0x3A) return;

    value = shift_down ? key_shift[scan] : key_base[scan];
    if (!value) return;
    if (input_count >= CONSOLE_INPUT_MAX) {
        input_dropped++;
        return;
    }
    input_queue[input_tail] = value;
    input_tail = (input_tail + 1) % CONSOLE_INPUT_MAX;
    input_count++;
    push_total++;
    wait_queue_wake_all(&input_wq);
}

int console_read(u8 *buf, u32 len)
{
    u32 out = 0;

    if (!buf || !len) return -1;
    while (out < len && input_count) {
        buf[out++] = input_queue[input_head];
        input_head = (input_head + 1) % CONSOLE_INPUT_MAX;
        input_count--;
    }
    if (!out) return -1;
    read_total += out;
    last_input = buf[out - 1];
    return (int)out;
}

static int console_read_ready(void *ctx)
{
    (void)ctx;
    return input_count != 0;
}

int console_read_block(u8 *buf, u32 len, u32 ticks)
{
    int rc;

    if (!buf || !len) return -1;
    if (!ticks) return console_read(buf, len);
    rc = task_block_on(&input_wq, console_read_ready, 0, ticks);
    if (rc < 0) return -1;
    if (rc > 0) return -2;
    return console_read(buf, len);
}

u32 console_queue_count(void)
{
    return input_count;
}

u32 console_input_dropped(void)
{
    return input_dropped;
}

u32 console_scan_total(void)
{
    return scan_total;
}

u32 console_push_total(void)
{
    return push_total;
}

int console_write(const u8 *buf, u32 len)
{
    u32 i;

    if (!buf || !len || len > CONSOLE_WRITE_MAX) return -1;
    uart_write(buf, len);
    uart_bytes += len;
    for (i = 0; i < len; i++) {
        console_put_byte(buf[i]);
        output_total++;
        output_bytes++;
        last_output = buf[i];
    }
    return (int)len;
}

u32 console_output_total(void)
{
    return output_total;
}

u32 console_output_bytes(void)
{
    return output_bytes;
}

u32 console_uart_bytes(void)
{
    return uart_bytes;
}

u32 console_read_total(void)
{
    return read_total;
}

u32 console_last_input(void)
{
    return last_input;
}

u32 console_last_output(void)
{
    return last_output;
}

u32 console_line_count(void)
{
    return output_line_count;
}

const char *console_line(u32 index)
{
    if (index >= output_line_count || index >= CONSOLE_LINES) return "";
    return output_lines[index];
}
