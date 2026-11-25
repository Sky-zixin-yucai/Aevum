; P12.1 image4：只读 ramfs 的最小文件 ABI 验收。
; 固定加载地址 0x400000；成功向 fd 1 写 P12.1 RAMFS OK，然后 exit(0)。
; 失败码 40..45，由 image3 父进程映射成退出码 30。
[bits 32]
section .text
global user_ramfs_exec_start
global user_ramfs_exec_end

user_ramfs_exec_start:
    ; open("hello.txt", 0) -> fd 2
    push byte 32
    pop eax
    mov ebx, hello_path - user_ramfs_exec_start + 0x400000
    xor ecx, ecx
    int 0x80
    cmp eax, 2
    jne .bad1
    mov [0x401000], eax

    ; read(fd, buf, 32) -> 26
    push byte 15
    pop eax
    mov ebx, [0x401000]
    mov ecx, 0x401020
    push byte 32
    pop edx
    int 0x80
    cmp eax, 26
    jne .bad2
    cmp dword [0x401020], 0x6C6C6548    ; "Hell"
    jne .bad2
    cmp byte [0x401039], 10             ; offset 25 = '\n'
    jne .bad2

    ; 再读必须 EOF 0（文件偏移已前进到 26）
    push byte 15
    pop eax
    mov ebx, [0x401000]
    mov ecx, 0x401020
    push byte 32
    pop edx
    int 0x80
    cmp eax, 0
    jne .bad3

    ; ramfs 只读：write 返回 -1
    push byte 16
    pop eax
    mov ebx, [0x401000]
    mov ecx, 0x401020
    push byte 1
    pop edx
    int 0x80
    cmp eax, -1
    jne .bad4

    ; close 返回 0；关闭后 read 返回 -1
    push byte 17
    pop eax
    mov ebx, [0x401000]
    int 0x80
    cmp eax, 0
    jne .bad5
    push byte 15
    pop eax
    mov ebx, [0x401000]
    mov ecx, 0x401020
    push byte 1
    pop edx
    int 0x80
    cmp eax, -1
    jne .bad5

    ; 不存在文件返回 -1
    push byte 32
    pop eax
    mov ebx, missing_path - user_ramfs_exec_start + 0x400000
    xor ecx, ecx
    int 0x80
    cmp eax, -1
    jne .bad6

    ; flags != 0（只接受 O_RDONLY=0）返回 -1
    push byte 32
    pop eax
    mov ebx, hello_path - user_ramfs_exec_start + 0x400000
    push byte 1
    pop ecx
    int 0x80
    cmp eax, -1
    jne .bad6

    ; open("motd.txt", 0) 复用 fd 2，读 30 字节
    push byte 32
    pop eax
    mov ebx, motd_path - user_ramfs_exec_start + 0x400000
    xor ecx, ecx
    int 0x80
    cmp eax, 2
    jne .bad7
    mov [0x401000], eax
    push byte 15
    pop eax
    mov ebx, [0x401000]
    mov ecx, 0x401020
    push byte 32
    pop edx
    int 0x80
    cmp eax, 30
    jne .bad7
    cmp dword [0x401020], 0x55564541    ; "AEVU"
    jne .bad7
    cmp byte [0x40103D], 10             ; offset 29 = '\n'
    jne .bad7
    push byte 17
    pop eax
    mov ebx, [0x401000]
    int 0x80
    cmp eax, 0
    jne .bad7

    ; 成功输出
    push byte 16
    pop eax
    push byte 1
    pop ebx
    mov ecx, ramfs_ok - user_ramfs_exec_start + 0x400000
    push byte 15
    pop edx
    int 0x80
    push byte 3
    pop eax
    xor ebx, ebx
    int 0x80
    jmp $

.bad1:
    push byte 40
    pop ebx
    jmp .fail
.bad2:
    push byte 41
    pop ebx
    jmp .fail
.bad3:
    push byte 42
    pop ebx
    jmp .fail
.bad4:
    push byte 43
    pop ebx
    jmp .fail
.bad5:
    push byte 44
    pop ebx
    jmp .fail
.bad6:
    push byte 45
    pop ebx
    jmp .fail
.bad7:
    push byte 46
    pop ebx
    jmp .fail
.fail:
    push byte 3
    pop eax
    int 0x80
    jmp $

hello_path:
    db 'hello.txt', 0
motd_path:
    db 'motd.txt', 0
missing_path:
    db 'missing.txt', 0
ramfs_ok:
    db 'P12.1 RAMFS OK', 10

user_ramfs_exec_end:
