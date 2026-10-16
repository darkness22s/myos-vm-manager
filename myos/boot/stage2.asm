; boot/stage2.asm - stage 2 (real mode, loaded at 0x0000:0x7E00 by stage 1)
;   1. enable the A20 address line
;   2. load the kernel to physical 0x10000 with INT 13h extended reads
;   3. switch to 32-bit protected mode with a flat GDT
;   4. copy the kernel to 0x100000 and jump to it

[BITS 16]
[ORG 0x7E00]

%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 64
%endif
%ifndef KERNEL_LBA
%define KERNEL_LBA 17
%endif

stage2:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl

    mov si, msg_loading
    call print16

    in al, 0x92                   ; enable A20 through the "fast A20" port
    or al, 0x02
    and al, 0xFE                  ; bit 0 would reset the CPU
    out 0x92, al

    ; Read the kernel one sector per BIOS call. Each sector goes to the next
    ; 512-byte slot, expressed as a segment step (0x20 paragraphs) at offset 0.
    mov word [kern_seg], 0x1000
    mov dword [kern_lba], KERNEL_LBA
    mov word [kern_left], KERNEL_SECTORS

.load:
    mov ax, [kern_seg]
    mov [dap_seg], ax
    mov word [dap_off], 0
    mov word [dap_count], 1
    mov eax, [kern_lba]
    mov [dap_lba], eax
    mov dword [dap_lba + 4], 0
    mov si, dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_fail
    add word [kern_seg], 0x20
    inc dword [kern_lba]
    dec word [kern_left]
    jnz .load

    mov si, msg_ok
    call print16

    ; Keep the boot path BIOS-only, then hand the kernel a 320x200x256
    ; framebuffer (VGA mode 13h) for the native desktop.
    mov ax, 0x0013
    int 0x10

    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:pm_entry       ; far jump: loads CS with the code selector

disk_fail:
    mov si, msg_fail
    call print16
.hang:
    cli
    hlt
    jmp .hang

; print the zero-terminated string at DS:SI using BIOS teletype output
print16:
    lodsb
    test al, al
    jz .done
    mov dx, 0x3F8                 ; also send to COM1 so serial logs show boot progress
    out dx, al
    mov ah, 0x0E
    mov bh, 0
    int 0x10
    jmp print16
.done:
    ret

; ---- 32-bit protected mode ----
[BITS 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    mov esi, 0x10000
    mov edi, 0x100000
    mov ecx, KERNEL_SECTORS * 512 / 4
    cld
    rep movsd

    mov eax, 0x100000
    jmp eax

; ---- data (lives inside this image; accessed from real mode) ----
[BITS 16]
boot_drive  db 0
kern_seg    dw 0
kern_left   dw 0
kern_lba    dd 0
msg_loading db "Loading kernel...", 13, 10, 0
msg_ok      db "Kernel loaded, entering protected mode", 13, 10, 0
msg_fail    db "Kernel load failed", 13, 10, 0

align 8
dap:
    db 0x10, 0
dap_count   dw 1
dap_off     dw 0
dap_seg     dw 0
dap_lba     dq 0

align 8
gdt:
    dq 0                          ; null descriptor
    dw 0xFFFF, 0x0000             ; code: base 0, limit 4 GiB, ring 0, 32-bit
    db 0x00, 0x9A, 0xCF, 0x00
    dw 0xFFFF, 0x0000             ; data: base 0, limit 4 GiB, ring 0
    db 0x00, 0x92, 0xCF, 0x00
gdt_end:

gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt
