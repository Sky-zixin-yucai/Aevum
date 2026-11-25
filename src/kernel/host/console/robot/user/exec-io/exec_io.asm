; exec_io.asm：P11.3 阻塞 I/O 用户镜像，固定运行在 0x400000。
; 独立镜像避开 ring3 的 4KB code VMA，验证：
;   1) 空 pipe + 活写端，read_block(2 tick) 超时返回 -2；
;   2) 子进程 sleep(20) 后写 4B，父 read_block(500 tick) 被唤醒并读回 IORD；
;   3) 父把 64B pipe 填满后 write_block(1B,500 tick) 阻塞，子 sleep(100) 后读 1B 唤醒。
; 全部成功写 "IO13"/exit(0)，否则输出 "IOBAD\n" 并带失败码退出。
[bits 32]
section .text
global user_io_exec_start
global user_io_exec_end

; data 0x401000: pipe1[read,write]
; 0x40100C read n, 0x401010 buffer, 0x401018 read child, 0x40101C status,
; 0x401020 wait ret, 0x401024 pipe2[read,write], 0x40102C fill count.

user_io_exec_start:
    push byte 14
    pop eax
    mov ebx, 0x401000
    int 0x80
    test eax, eax
    jnz .bad1

    ; 空 pipe，写端仍活着：2 tick 必须超时 -2。
    push byte 28
    pop eax
    mov ebx, [0x401000]
    mov ecx, 0x401010
    push byte 4
    pop edx
    push byte 2
    pop esi
    int 0x80
    cmp eax, -2
    jne .bad2

    push byte 17
    pop eax
    mov ebx, [0x401000]
    int 0x80
    push byte 17
    pop eax
    mov ebx, [0x401004]
    int 0x80

    ; 读阻塞 + 子写端唤醒。
    push byte 14
    pop eax
    mov ebx, 0x401000
    int 0x80
    test eax, eax
    jnz .bad3

    mov dword [0x401010], 0x44524F49   ; "IORD" payload
    push byte 6
    pop eax
    int 0x80
    test eax, eax
    jz .read_child

    mov [0x401018], eax
    push byte 17
    pop eax
    mov ebx, [0x401004]
    int 0x80

    push byte 28
    pop eax
    mov ebx, [0x401000]
    mov ecx, 0x401010
    push byte 4
    pop edx
    mov esi, 500
    int 0x80
    cmp eax, 4
    jne .bad4
    cmp dword [0x401010], 0x44524F49   ; "IORD"
    jne .bad5

    push byte 4
    pop eax
    mov ebx, [0x401018]
    mov ecx, 0x40101C
    int 0x80
    cmp dword [0x40101C], 0
    jne .bad6
    push byte 17
    pop eax
    mov ebx, [0x401000]
    int 0x80

    ; 写阻塞 + 子读端唤醒：先把 64B 填满。
    push byte 14
    pop eax
    mov ebx, 0x401024
    int 0x80
    test eax, eax
    jnz .bad7
    push byte 6
    pop eax
    int 0x80
    test eax, eax
    jz .write_child

    mov [0x401034], eax

    mov dword [0x4010F0], 0x44434241   ; "ABCD"
    mov dword [0x4010F4], 0x48474645   ; "EFGH"
    push byte 8
    pop eax
    mov [0x40102C], eax
.fill:
    push byte 16
    pop eax
    mov ebx, [0x401028]
    mov ecx, 0x4010F0
    push byte 8
    pop edx
    int 0x80
    cmp eax, 8
    jne .bad8
    dec dword [0x40102C]
    jnz .fill

    push byte 29
    pop eax
    mov ebx, [0x401028]
    mov ecx, 0x401010
    push byte 1
    pop edx
    mov esi, 500
    int 0x80
    cmp eax, 1
    jne .bad9

    push byte 4
    pop eax
    mov ebx, [0x401034]
    mov ecx, 0x401038
    int 0x80
    cmp dword [0x401038], 0
    jne .bad10
    push byte 17
    pop eax
    mov ebx, [0x401028]
    int 0x80

    mov dword [0x401040], 0x33314F49  ; "IO13"
    push byte 16
    pop eax
    push byte 1
    pop ebx
    mov ecx, io_ok + 0x400000
    push byte 12
    pop edx
    int 0x80
    push byte 3
    pop eax
    xor ebx, ebx
    int 0x80
    jmp $

.read_child:
    push byte 17
    pop eax
    mov ebx, [0x401000]
    int 0x80
    push byte 24
    pop eax
    push byte 20
    pop ebx
    int 0x80
    push byte 16
    pop eax
    mov ebx, [0x401004]
    mov ecx, 0x401010
    push byte 4
    pop edx
    int 0x80
    cmp eax, 4
    jne .child_bad1
    push byte 3
    pop eax
    xor ebx, ebx
    int 0x80
    jmp $

.write_child:
    push byte 17
    pop eax
    mov ebx, [0x401028]
    int 0x80
    push byte 24
    pop eax
    push byte 100
    pop ebx
    int 0x80
    push byte 15
    pop eax
    mov ebx, [0x401024]
    mov ecx, 0x401010
    push byte 1
    pop edx
    int 0x80
    cmp eax, 1
    jne .child_bad2
    push byte 3
    pop eax
    xor ebx, ebx
    int 0x80
    jmp $

.child_bad1:
    push byte 11
    pop ebx
    jmp .fail
.child_bad2:
    push byte 12
    pop ebx
    jmp .fail
.bad1:
    push byte 1
    pop ebx
    jmp .fail
.bad2:
    push byte 2
    pop ebx
    jmp .fail
.bad3:
    push byte 3
    pop ebx
    jmp .fail
.bad4:
    push byte 4
    pop ebx
    jmp .fail
.bad5:
    push byte 5
    pop ebx
    jmp .fail
.bad6:
    push byte 6
    pop ebx
    jmp .fail
.bad7:
    push byte 7
    pop ebx
    jmp .fail
.bad8:
    push byte 8
    pop ebx
    jmp .fail
.bad9:
    push byte 9
    pop ebx
    jmp .fail
.bad10:
    mov ebx, 10
.fail:
    push ebx
    push byte 16
    pop eax
    push byte 1
    pop ebx
    mov ecx, io_bad + 0x400000
    push byte 6
    pop edx
    int 0x80
    push byte 3
    pop eax
    pop ebx
    int 0x80
    jmp $

io_word:
    db 'I', 'O', 'R', 'D'
io_fill:
    db 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'
io_ok:
    db 'P11.3 IO OK', 10
io_bad:
    db 'IOBAD', 10

user_io_exec_end:
