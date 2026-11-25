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

    ; P11.6：三个读者按 20/40/60 tick 依次阻塞在同一 pipe 读端。
    ; 父每 50 tick 写一个 token；FIFO 正确时子进程按 1/2/3 回写结果。
    push byte 14
    pop eax
    mov ebx, 0x401100
    int 0x80
    test eax, eax
    jnz .bad13
    push byte 14
    pop eax
    mov ebx, 0x401108
    int 0x80
    test eax, eax
    jnz .bad14

    ; 子进程 1：20 tick 后阻塞，持有 id '1'。
    mov dword [0x40111C], 20
    mov dword [0x401120], 0x31
    push byte 6
    pop eax
    int 0x80
    test eax, eax
    js .bad14
    jz .fifo_child
    mov [0x401110], eax

    ; 子进程 2：40 tick 后阻塞，持有 id '2'。
    mov dword [0x40111C], 40
    mov dword [0x401120], 0x32
    push byte 6
    pop eax
    int 0x80
    test eax, eax
    js .bad14
    jz .fifo_child
    mov [0x401114], eax

    ; 子进程 3：60 tick 后阻塞，持有 id '3'。
    mov dword [0x40111C], 60
    mov dword [0x401120], 0x33
    push byte 6
    pop eax
    int 0x80
    test eax, eax
    js .bad14
    jz .fifo_child
    mov [0x401118], eax

    ; 父只保留 data 写端和 result 读端，避免自己持有对端。
    push byte 17
    pop eax
    mov ebx, [0x401100]
    int 0x80
    push byte 17
    pop eax
    mov ebx, [0x40110C]
    int 0x80

    ; 先收三个 'R'，确认每个子进程都已走到阻塞入口附近。
    mov dword [0x40112C], 0
.fifo_ready_loop:
    mov eax, [0x40112C]
    mov ecx, 0x401124
    add ecx, eax
    mov edx, 3
    sub edx, eax
    push byte 28
    pop eax
    mov ebx, [0x401108]
    mov esi, 500
    int 0x80
    cmp eax, 1
    jl .bad15
    add [0x40112C], eax
    cmp dword [0x40112C], 3
    jb .fifo_ready_loop
    cmp byte [0x401124], 0x52
    jne .bad15
    cmp byte [0x401125], 0x52
    jne .bad15
    cmp byte [0x401126], 0x52
    jne .bad15

    push byte 24
    pop eax
    push byte 30
    pop ebx
    int 0x80

    ; 每 50 tick 写一个 A/B/C，只允许队首读者被唤醒并取走。
    mov dword [0x40112C], 0
.fifo_token_loop:
    mov al, 0x41
    add al, byte [0x40112C]
    mov [0x401130], al
    push byte 16
    pop eax
    mov ebx, [0x401104]
    mov ecx, 0x401130
    push byte 1
    pop edx
    int 0x80
    cmp eax, 1
    jne .bad16
    push byte 24
    pop eax
    push byte 50
    pop ebx
    int 0x80
    inc dword [0x40112C]
    cmp dword [0x40112C], 3
    jb .fifo_token_loop

    ; 读三个回写 id，顺序必须是 1/2/3。
    mov dword [0x40112C], 0
.fifo_order_loop:
    mov eax, [0x40112C]
    mov ecx, 0x401128
    add ecx, eax
    mov edx, 3
    sub edx, eax
    push byte 28
    pop eax
    mov ebx, [0x401108]
    mov esi, 500
    int 0x80
    cmp eax, 1
    jl .bad17
    add [0x40112C], eax
    cmp dword [0x40112C], 3
    jb .fifo_order_loop
    cmp byte [0x401128], 0x31
    jne .bad18
    cmp byte [0x401129], 0x32
    jne .bad18
    cmp byte [0x40112A], 0x33
    jne .bad18

    ; 三个子进程都必须正常退出。
    mov dword [0x40112C], 0
.fifo_wait_loop:
    mov ecx, [0x40112C]
    mov ebx, [0x401110 + ecx*4]
    push byte 4
    pop eax
    mov ecx, 0x401138
    int 0x80
    cmp eax, 0
    jle .bad19
    cmp dword [0x401138], 0
    jne .bad19
    inc dword [0x40112C]
    cmp dword [0x40112C], 3
    jb .fifo_wait_loop

    push byte 17
    pop eax
    mov ebx, [0x401104]
    int 0x80
    push byte 17
    pop eax
    mov ebx, [0x401108]
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
    push byte 16
    pop eax
    push byte 1
    pop ebx
    mov ecx, wq_ok + 0x400000
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

; P11.6 FIFO 子进程：延迟到确定的先后顺序后阻塞在同一读端，读到一个 token
; 就把自己的 id 写回 result pipe；读失败写 'X' 并 exit(1)。
.fifo_child:
    push byte 17
    pop eax
    mov ebx, [0x401104]
    int 0x80
    push byte 17
    pop eax
    mov ebx, [0x401108]
    int 0x80

    mov ebx, [0x40111C]
    push byte 24
    pop eax
    int 0x80

    mov byte [0x401130], 0x52   ; "R"：已到达阻塞入口
    push byte 16
    pop eax
    mov ebx, [0x40110C]
    mov ecx, 0x401130
    push byte 1
    pop edx
    int 0x80
    cmp eax, 1
    jne .fifo_child_fail

    push byte 28
    pop eax
    mov ebx, [0x401100]
    mov ecx, 0x401134
    push byte 1
    pop edx
    mov esi, 500
    int 0x80
    cmp eax, 1
    jne .fifo_child_badread

    mov al, [0x401120]
    mov [0x401130], al
    push byte 16
    pop eax
    mov ebx, [0x40110C]
    mov ecx, 0x401130
    push byte 1
    pop edx
    int 0x80
    cmp eax, 1
    jne .fifo_child_fail

    push byte 17
    pop eax
    mov ebx, [0x401100]
    int 0x80
    push byte 17
    pop eax
    mov ebx, [0x40110C]
    int 0x80
    push byte 3
    pop eax
    xor ebx, ebx
    int 0x80
    jmp $

.fifo_child_badread:
    mov byte [0x401130], 0x58   ; "X"：超时或读失败
    push byte 16
    pop eax
    mov ebx, [0x40110C]
    mov ecx, 0x401130
    push byte 1
    pop edx
    int 0x80
.fifo_child_fail:
    push byte 3
    pop eax
    push byte 1
    pop ebx
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
    jmp .fail
.bad13:
    mov ebx, 13
    jmp .fail
.bad14:
    mov ebx, 14
    jmp .fail
.bad15:
    mov ebx, 15
    jmp .fail
.bad16:
    mov ebx, 16
    jmp .fail
.bad17:
    mov ebx, 17
    jmp .fail
.bad18:
    mov ebx, 18
    jmp .fail
.bad19:
    mov ebx, 19
    jmp .fail
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
wq_ok:
    db 'P11.6 WQ OK', 10
io_bad:
    db 'IOBAD', 10

user_io_exec_end:
