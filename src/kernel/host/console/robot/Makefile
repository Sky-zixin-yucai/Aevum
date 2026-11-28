# Makefile —— 32 位裸机内核（模块化目录）
# 依赖：nasm / gcc(支持 -m32 编译) / ld(binutils) / make / qemu-system-i386

CC      = gcc
AS      = nasm
LD      = ld
QEMU    = qemu-system-i386
HOST_CC = cc

BUILD   = build
DEFS    =
# 非 FPU 模块禁止编译器生成 SSE 暂存，避免打乱 fxsave 捕获任务 XMM 状态；
# FPU 演示对象单独追加 -msse2（见下方 target-specific CFLAGS）。
CFLAGS  = -m32 -mno-sse -ffreestanding -c -Os -g -fno-tree-loop-distribute-patterns -fno-pic -fno-pie \
          -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -falign-functions=1 -falign-jumps=1 -falign-loops=1 \
          -ffunction-sections -fdata-sections -Wall -Wextra -Iinclude -I. $(DEFS)
ASFLAGS_BIN   = -f bin
ASFLAGS_ELF32 = -f elf32
LDSCRIPT = kernel.ld

SRCS = \
	kernel/main/main.c \
	kernel/error/error.c \
	kernel/task/task.c \
	kernel/task/stack/stack.c \
	kernel/task/kthread/kthread.c \
	kernel/task/family/family.c \
	kernel/proc/process.c \
	kernel/proc/fd/fd.c \
	kernel/fs/ramfs/ramfs.c \
	kernel/ipc/pipe/pipe.c \
	kernel/mm/address_space/address_space.c \
	arch/x86/cpu/interrupt/interrupt.c \
	arch/x86/cpu/lapic/lapic.c \
	arch/x86/cpu/tss/tss.c \
	arch/x86/cpu/syscall/syscall.c \
	arch/x86/cpu/fault/page_fault.c \
	arch/x86/cpu/exception/exception.c \
	drivers/video/vga/text/vga.c \
	drivers/serial/uart/uart.c \
	drivers/input/keyboard/keyboard.c \
	drivers/console/console.c \
	lib/fixed/fixed.c \
	lib/math/trig.c \
	lib/format/format.c \
	lib/geom/line.c \
	lib/geom/arc.c \
	app/path/path.c \
	app/arm/app/app.c \
	app/arm/kinematics/kinematics.c \
	app/arm/control/control.c \
	app/arm/render/render.c \
	app/fpu/demo/demo.c \
	app/fpu/render/render.c \
	app/tree/demo/demo.c \
	app/tree/render/render.c \
	app/user/demo/demo.c \
	app/user/page/page.c \
	app/user/render/render.c \
	app/user/render/draw.c \
	app/user/render/slots.c \
	app/user/render/tree.c \
	app/user/render/address.c \
	app/user/render/fd.c \
	app/console/demo/demo.c \
	app/console/render/render.c \
	app/pool/demo/demo.c \
	app/pool/render/render.c \
	app/bench/bench.c \
	app/rtbench/rtbench.c

OBJS    = $(addprefix $(BUILD)/,$(SRCS:.c=.o)) $(BUILD)/user/ring3.o $(BUILD)/user/exec.o $(BUILD)/user/exec_pipe.o $(BUILD)/user/exec_io.o $(BUILD)/user/exec_ramfs.o $(BUILD)/kernel/fs/ramfs/ramdisk.o
IMG     = os.img

GENERATED = boot.bin $(IMG) disk.img kernel.bin kernel.elf $(BUILD)

.PHONY: all clean rebuild run debug size test-host

all: disk.img

# FPU 演示是唯一需要编译器显式按 SSE2 编译的模块；其余模块保持 -mno-sse。
$(BUILD)/app/fpu/demo/demo.o: CFLAGS += -msse2

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/loader.o: loader.asm
	@mkdir -p $(BUILD)
	$(AS) $(ASFLAGS_ELF32) loader.asm -o $@

$(BUILD)/user/ring3.o: user/ring3/ring3.asm
	@mkdir -p $(BUILD)/user
	$(AS) $(ASFLAGS_ELF32) user/ring3/ring3.asm -o $@

$(BUILD)/user/exec.o: user/exec/exec.asm
	@mkdir -p $(BUILD)/user
	$(AS) $(ASFLAGS_ELF32) user/exec/exec.asm -o $@

$(BUILD)/user/exec_pipe.o: user/exec-pipe/exec_pipe.asm
	@mkdir -p $(BUILD)/user
	$(AS) $(ASFLAGS_ELF32) user/exec-pipe/exec_pipe.asm -o $@

$(BUILD)/user/exec_io.o: user/exec-io/exec_io.asm
	@mkdir -p $(BUILD)/user
	$(AS) $(ASFLAGS_ELF32) user/exec-io/exec_io.asm -o $@

$(BUILD)/user/exec_ramfs.o: user/exec-ramfs/exec_ramfs.asm
	@mkdir -p $(BUILD)/user
	$(AS) $(ASFLAGS_ELF32) user/exec-ramfs/exec_ramfs.asm -o $@

$(BUILD)/kernel/fs/ramfs/ramdisk.o: kernel/fs/ramfs/ramdisk.asm
	@mkdir -p $(BUILD)/kernel/fs/ramfs
	$(AS) $(ASFLAGS_ELF32) kernel/fs/ramfs/ramdisk.asm -o $@

kernel.elf: $(OBJS) $(BUILD)/loader.o $(LDSCRIPT)
	$(LD) -m elf_i386 --gc-sections -T $(LDSCRIPT) $(BUILD)/loader.o $(OBJS) -o kernel.elf

# 由同一个 ELF 产出 flat binary，保证 loader 与 C 看到的 __bss_start 完全一致。
kernel.bin: kernel.elf
	objcopy -O binary kernel.elf kernel.bin
	@size=$$(wc -c < kernel.bin); echo "kernel.bin bytes: $$size (limit 118784 / 232 sectors)"; \
	if [ $$size -gt 118784 ]; then echo "ERROR: kernel.bin $$size bytes > 118784"; exit 1; fi

boot.bin: boot.asm boot_common.inc
	$(AS) $(ASFLAGS_BIN) boot.asm -o boot.bin

$(IMG): boot.bin kernel.bin
	dd if=/dev/zero of=$(IMG) bs=512 count=2880
	dd if=boot.bin of=$(IMG) conv=notrunc
	dd if=kernel.bin of=$(IMG) bs=512 seek=1 conv=notrunc

# 桌面端固定读 disk.img；和 os.img 内容一致，保留 os.img 便于直接 qemu 调试。
disk.img: $(IMG)
	cp $(IMG) disk.img

size: kernel.bin
	@wc -c kernel.bin

clean:
	rm -rf $(GENERATED)

# 纯几何/路径函数在宿主机上用原生工具链编译测试；不产出内核镜像。
test-host:
	@mkdir -p $(BUILD)/host
	$(HOST_CC) -std=c99 -Wall -Wextra -Iinclude \
		tests/host/line_test.c lib/geom/line.c lib/fixed/fixed.c \
		-o $(BUILD)/host/line_test
	$(HOST_CC) -std=c99 -Wall -Wextra -Iinclude \
		tests/host/arc_test.c lib/geom/arc.c lib/math/trig.c lib/fixed/fixed.c \
		-o $(BUILD)/host/arc_test
	$(HOST_CC) -std=c99 -Wall -Wextra -Iinclude \
		tests/host/path_line_test.c app/path/path.c lib/geom/line.c lib/geom/arc.c \
		lib/math/trig.c lib/fixed/fixed.c \
		-o $(BUILD)/host/path_line_test
	$(HOST_CC) -std=c99 -Wall -Wextra -Iinclude \
		tests/host/path_arc_test.c app/path/path.c lib/geom/line.c lib/geom/arc.c \
		lib/math/trig.c lib/fixed/fixed.c \
		-o $(BUILD)/host/path_arc_test
	$(BUILD)/host/line_test
	$(BUILD)/host/arc_test
	$(BUILD)/host/path_line_test
	$(BUILD)/host/path_arc_test

rebuild:
	$(MAKE) clean
	$(MAKE) all

run: disk.img
	$(QEMU) -drive format=raw,file=disk.img

debug: $(IMG)
	$(QEMU) -s -S -drive format=raw,file=$(IMG)
