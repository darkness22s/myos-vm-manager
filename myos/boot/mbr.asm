; boot/mbr.asm - stage 1
; The BIOS loads this 512-byte sector to 0x0000:0x7C00 and passes the boot
; drive number in DL. It reads stage 2 (16 sectors at LBA 1) to 0x0000:0x7E00
; and jumps there. The partition entry below describes the FAT16 volume that
; starts at LBA 2048.

[BITS 16]
[ORG 0x7C00]

%ifndef PART_SECTORS
%define PART_SECTORS 30720
%endif

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [boot_drive], dl

    mov dl, [boot_drive]
    mov si, dap
    mov ah, 0x42                  ; INT 13h extended read (LBA addressing)
    int 0x13
    jc .fail

    mov dl, [boot_drive]          ; stage 2 expects the drive number in DL
    jmp 0x0000:0x7E00

.fail:
    mov si, msg_fail
.print:
    lodsb
    test al, al
    jz .hang
    mov ah, 0x0E
    mov bh, 0
    int 0x10
    jmp .print
.hang:
    cli
    hlt
    jmp .hang

msg_fail    db "disk read error", 0
boot_drive  db 0

dap:
    db 0x10                       ; size of this packet
    db 0
    dw 16                         ; sectors to read
    dw 0x7E00                     ; destination offset
    dw 0x0000                     ; destination segment
    dq 1                          ; first LBA

times 0x1BE - ($ - $$) db 0       ; pad up to the partition table

; One partition entry: active, type 0x04 (FAT16 under 32 MiB), start LBA 2048.
    db 0x80
    db 0xFE, 0xFF, 0xFF           ; CHS start (unused with LBA addressing)
    db 0x04
    db 0xFE, 0xFF, 0xFF           ; CHS end (unused with LBA addressing)
    dd 2048
    dd PART_SECTORS

times 48 db 0
dw 0xAA55
