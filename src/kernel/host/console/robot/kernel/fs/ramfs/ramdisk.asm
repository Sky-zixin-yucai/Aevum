; P12.1 只读 ramdisk 镜像（链接进 .rodata，不依赖磁盘驱动）。
; 布局：16B 头 + N×24B 目录项 + 文件区。
;   magic "AEVR"(0x52564541), version=1, count, reserved
;   entry: name[16] (NUL 结尾), file_offset u32, file_size u32
; 文件名只支持单层根目录名；内容不可写。
[bits 32]
section .rodata
global ramdisk_start
global ramdisk_end
align 4

ramdisk_start:
    dd 0x52564541          ; "AEVR"
    dd 1                   ; version
    dd 2                   ; file count
    dd 0                   ; reserved

    ; entry 0: hello.txt
    db 'hello.txt'
    times 7 db 0
    dd hello_file - ramdisk_start
    dd hello_file_end - hello_file

    ; entry 1: motd.txt
    db 'motd.txt'
    times 8 db 0
    dd motd_file - ramdisk_start
    dd motd_file_end - motd_file

hello_file:
    db 'Hello from Aevum ramdisk!', 10
hello_file_end:

motd_file:
    db 'AEVUM P12.1 READ-ONLY RAMDISK', 10
motd_file_end:

ramdisk_end:
