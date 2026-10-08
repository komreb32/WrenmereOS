/* SPDX-License-Identifier: BSD-3-Clause */
.intel_syntax noprefix
.code16

.equ Stage2Sectors, 32

.global Start
Start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov byte ptr [BootDrive], dl

    mov si, offset DiskPacket
    mov ah, 0x42
    mov dl, [BootDrive]
    int 0x13
    jc DiskError

    mov dl, [BootDrive]
    ljmp 0x0000, 0x8000

DiskError:
    mov bx, 0x0007
    mov ah, 0x0E
    mov al, 'E'
    int 0x10

HaltLoop:
    hlt
    jmp HaltLoop

DiskPacket:
    .byte 0x10              # DAP size
    .byte 0x00              # reserved
    .word Stage2Sectors
    .word 0x8000            # offset
    .word 0x0000            # segment
    .quad 1                 # LBA

BootDrive:
    .byte 0x00

.org 510
.word 0xAA55
