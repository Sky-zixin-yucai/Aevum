; robot/boot.asm — 唯一内核引导扇区。
; 规则: 读盘失败必须打印可见消息；只负责把 kernel.bin 装到 0x80000。
; 公共的 GDT/保护模式/恒等分页在同目录 boot_common.inc。
[bits 16]
[org 0x7c00]

%include "boot_common.inc"

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    ; 232 个扇区分两次 LBA 读到 0x80000/0x90000：BIOS 单次读上限
    ; 常见为 127/128，一次读会失败。窗口上限 118,784 字节，到 0x9D000。
    mov ah, 0x42
    mov si, dap
    mov dl, 0x80
    int 0x13
    jc disk_error
    mov ah, 0x42
    mov si, dap2
    mov dl, 0x80
    int 0x13
    jc disk_error
    jmp boot_load_ok

disk_error:
    mov si, msg_disk_error
    call boot_print_rm
    cli
    hlt

boot_load_ok:
    BOOT_ENTER_PM
    BOOT_BUILD_PAGETABLE
    BOOT_ENABLE_PAGING
    jmp BOOT_GDT_CODE_SEL:0x80000

align 4
dap:
    db 0x10, 0
    dw 128
    dw 0x0000
    dw 0x8000
    dq 1

dap2:
    db 0x10, 0
    dw 104
    dw 0x0000
    dw 0x9000
    dq 129

msg_disk_error: db "AEVUM robot boot: disk read failed", 0

; 数据与 16 位打印例程放在执行流之外
BOOT_GDT_DATA
BOOT_PRINT_RM

times 510 - ($-$$) db 0
dw 0xaa55
