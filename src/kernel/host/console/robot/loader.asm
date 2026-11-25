; loader.asm [bits 32]
[bits 32]
global start
global irq0_stub
global int40_stub
global int80_stub
global page_fault_stub
global exception_de_stub
global exception_bp_stub
global exception_ud_stub
global exception_gp_stub
global gdt_new
extern kernel_main
extern task_schedule_irq
extern syscall_dispatch
extern process_signal_deliver_async
extern page_fault_dispatch
extern exception_dispatch
extern __bss_start
extern __bss_end

; 中断帧的公共保存/恢复片段。ring3 帧比 ring0 帧多 CPU 自动压入的 esp/ss，
; 但 iret 会按目标 CS 的 RPL 自行决定是否弹出这两个槽，所以同一段恢复代码通用。
%macro SAVE_CTX 2
    push dword %1
    push dword %2
    push ebp
    push edi
    push esi
    push edx
    push ecx
    push ebx
    push eax
%endmacro

; 异常帧：CPU 已经压入 error code，stub 只补 vector，不再补 err。
%macro SAVE_EXC 1
    push dword %1
    push ebp
    push edi
    push esi
    push edx
    push ecx
    push ebx
    push eax
%endmacro

%macro RESTORE_CTX 0
    pop eax
    pop ebx
    pop ecx
    pop edx
    pop esi
    pop edi
    pop ebp
    add esp, 8
    iret
%endmacro

section .text
start:
    ; 打开 SSE 状态保存：CR4.OSFXSR/OSXMMEXCPT，清 CR0.EM/TS，置 MP。
    mov eax, cr4
    or eax, 0x600
    mov cr4, eax
    mov eax, cr0
    and eax, 0xFFFFFFF3
    or eax, 0x2
    mov cr0, eax

    ; BSS 可能越过 boot 读取的扇区窗口，加载后必须由内核自己清零。
    cld
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    ; 内核自己的 GDT：ring0 代码/数据、ring3 代码/数据、TSS 槽。
    ; TSS 描述符由 C 侧 tss_init() 写入，loader 只负责加载表和 ltr。
    lgdt [gdtr_new]

    mov esp, 0xC000
    call kernel_main

hang_loop:
    hlt
    jmp hang_loop

irq0_stub:
    SAVE_CTX 0, 0x20
    jmp task_common

; 软件调度门：task_yield/task_block/task_exit/task_start 共用。
int40_stub:
    SAVE_CTX 0, 0x40
    jmp task_common

; ring3 系统调用门：不经过调度器，由 syscall_dispatch 处理并返回同一帧。
int80_stub:
    SAVE_CTX 0, 0x80
    mov eax, esp
    push eax
    call syscall_dispatch
    add esp, 4
    mov esp, eax
    mov eax, esp
    push eax
    call process_signal_deliver_async
    add esp, 4
    mov esp, eax
    RESTORE_CTX

; #PF：CPU 压 error code；C 侧记录 CR2/EIP/ERR，再决定杀用户任务或停机。
page_fault_stub:
    SAVE_EXC 0x0E
    mov eax, esp
    push eax
    call page_fault_dispatch
    add esp, 4
    mov esp, eax
    RESTORE_CTX

; #DE/#BP/#UD 不压 error code，stub 先补 err=0；#GP 由 CPU 压 error code。
exception_de_stub:
    push dword 0
    SAVE_EXC 0x00
    jmp exception_common

exception_bp_stub:
    push dword 0
    SAVE_EXC 0x03
    jmp exception_common

exception_ud_stub:
    push dword 0
    SAVE_EXC 0x06
    jmp exception_common

exception_gp_stub:
    SAVE_EXC 0x0D
    jmp exception_common

exception_common:
    mov eax, esp
    push eax
    call exception_dispatch
    add esp, 4
    mov esp, eax
    RESTORE_CTX

task_common:
    mov eax, esp
    push eax
    call task_schedule_irq
    add esp, 4
    mov esp, eax
    mov eax, esp
    push eax
    call process_signal_deliver_async
    add esp, 4
    mov esp, eax
    RESTORE_CTX

; 内核 GDT 位于内核映像 .data，任务栈从 0x8000 起，不会覆盖。
section .data
align 8
gdt_new:
    dq 0
    dq 0x00CF9A000000FFFF      ; 0x08 ring0 code
    dq 0x00CF92000000FFFF      ; 0x10 ring0 data
    dq 0x00CFFA000000FFFF      ; 0x18 ring3 code -> 0x1B
    dq 0x00CFF2000000FFFF      ; 0x20 ring3 data -> 0x23
    dq 0                        ; 0x28 TSS，由 tss_init() 填写
gdt_new_end:
gdtr_new:
    dw gdt_new_end - gdt_new - 1
    dd gdt_new
