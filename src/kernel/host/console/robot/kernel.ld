/* kernel.ld 链接脚本
 * 内核加载到 0x80000，避开 BIOS 低内存暂存区；boot 用 224 个 LBA 扇区读到
 * 0x80000..0x9C000。BSS 由 loader 清零，页目录/页表仍在 0x10000/0x11000。
 */
ENTRY(start);

SECTIONS {
    . = 0x80000; /* 加载到物理地址 0x80000，避开 BIOS 低内存与 1MB 回绕 */

    .text : {
        *(.boot)
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .eh_frame : {
        *(.eh_frame)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        . = ALIGN(4);
        __bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        . = ALIGN(4);
        __bss_end = .;
    }
}
