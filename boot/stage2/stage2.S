/* SPDX-License-Identifier: BSD-3-Clause */
.intel_syntax noprefix
.code16

# NOTE: .code16 + .intel_syntax mis-assembles 32-bit memory operands,
# so every dword access in LoadKernel is emitted as raw bytes.

.ifndef Fs2StartLba
.equ Fs2StartLba, 2048
.endif
.equ KernelPhysBase, 0x400000
.equ KernelBufferSeg, 0x1000
.equ MaxBootBlocks, 768
.equ ChunkSectors, 64

.section .text.entry, "ax"
.global Stage2Start
Stage2Start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov byte ptr [BootDrive], dl

    call EnableA20
    call DetectMemory       # E820 map -> MemMap/Count before leaving real mode
    call LoadKernel         # 2fs superblock + contiguous kernel -> 0x400000
    jmp EnterProtectedMode

.section .text, "ax"
EnableA20:
    in al, 0x92
    or al, 0x02
    and al, 0xFE        # don't pulse the reset bit
    out 0x92, al
    ret

# Ask the BIOS for the usable RAM map. Entries land at 0x5000,
# count at 0x4FF0; both are identity mapped later so the kernel
# can read them in long mode.
.equ MemMapAddr, 0x5000
.equ MemInfoAddr, 0x4FF0
DetectMemory:
    xor ax, ax
    mov es, ax
    mov di, MemMapAddr
    xor ebx, ebx
    xor bp, bp              # entry count in bp
NextEntry:
    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150     # 'SMAP'
    int 0x15
    jc MemoryDone
    cmp eax, 0x534D4150
    jne MemoryDone
    cmp ecx, 20
    jb SkipEntry
    cmp dword ptr es:[di+16], 1  # type 1 = usable RAM
    jne SkipEntry
    add di, 24
    inc bp
    cmp bp, 32              # buffer holds 32 entries
    jae MemoryDone
SkipEntry:
    test ebx, ebx
    jnz NextEntry
MemoryDone:
    mov word ptr [MemInfoAddr], bp
    ret

# Load the contiguous EXF kernel to 0x400000 while still in real mode.
# Uses the drive number saved from DL at entry. INT 13h AH=0x42 reads
# through one DAP at a time; INT 15h AH=0x87 copies each chunk above 1MB.
LoadKernel:
    # 1) Read 1 sector at LBA Fs2StartLba (the 2fs superblock) to 0x10000.
    mov word ptr [LoadDapCount], 1
    mov word ptr [LoadDapOffset], 0x0000
    mov word ptr [LoadDapSegment], KernelBufferSeg
    .byte 0x66, 0xC7, 0x06
    .word LoadDapLba
    .long Fs2StartLba
    .byte 0x66, 0xC7, 0x06
    .word LoadDapLba + 4
    .long 0
    mov si, offset LoadDap
    mov ah, 0x42
    mov dl, byte ptr [BootDrive]
    int 0x13
    jc LoadKernelKError

    # 2) Check the 2fs magic ("2FS-WREN") and BlockSize == 4096.
    # DS stays 0 (variables); ES points at the 0x10000 buffer.
    push es
    mov ax, KernelBufferSeg
    mov es, ax
    .byte 0x26, 0x66, 0x81, 0x3E, 0x00, 0x00  # cmp dword es:[0], ...
    .long 0x2D534632
    jne LoadKernelSPopFail
    .byte 0x26, 0x66, 0x81, 0x3E, 0x04, 0x00  # cmp dword es:[4], ...
    .long 0x4E455257
    jne LoadKernelSPopFail
    .byte 0x26, 0x66, 0x81, 0x3E, 0x0C, 0x00  # cmp dword es:[12], ...
    .long 4096
    jne LoadKernelSPopFail
    # 3) BootFileBlock (128), BootFileBlocks (136), BootFileSize (144),
    # only the low 32 bits are used.
    .byte 0x26, 0x66, 0xA1, 0x80, 0x00        # mov eax, es:[0x80]
    .byte 0x66, 0xA3
    .word BootFileBlockLo
    .byte 0x26, 0x66, 0xA1, 0x84, 0x00        # mov eax, es:[0x84]
    .byte 0x66, 0x09, 0xC0                    # or eax, eax
    jnz LoadKernelSPopFailK
    .byte 0x26, 0x66, 0xA1, 0x88, 0x00        # mov eax, es:[0x88]
    .byte 0x66, 0xA3
    .word BootFileBlocksLo
    .byte 0x26, 0x66, 0xA1, 0x8C, 0x00        # mov eax, es:[0x8C]
    .byte 0x66, 0x09, 0xC0                    # or eax, eax
    jnz LoadKernelSPopFailK
    .byte 0x26, 0x66, 0xA1, 0x90, 0x00        # mov eax, es:[0x90]
    .byte 0x66, 0xA3
    .word KernelFileSize
    .byte 0x26, 0x66, 0xA1, 0x94, 0x00        # mov eax, es:[0x94]
    .byte 0x66, 0x09, 0xC0                    # or eax, eax
    jnz LoadKernelSPopFailK
    pop es
    .byte 0x66, 0xA1
    .word BootFileBlocksLo
    .byte 0x66, 0x09, 0xC0                    # or eax, eax
    jz LoadKernelKError
    .byte 0x66, 0x3D
    .long MaxBootBlocks
    ja LoadKernelKError

    # 4) LBA = Fs2StartLba + BootFileBlock*8; sectors = BootFileBlocks*8.
    xor edx, edx                         # clear high half before adc/test
    .byte 0x66, 0xA1
    .word BootFileBlockLo
    .byte 0x66, 0xC1, 0xE0, 0x03              # shl eax, 3
    .byte 0x66, 0x05
    .long Fs2StartLba
    .byte 0x66, 0x83, 0xD2, 0x00              # adc edx, 0
    .byte 0x66, 0x85, 0xD2                    # test edx, edx
    jnz LoadKernelKError
    .byte 0x66, 0xA3
    .word KernelLba
    .byte 0x66, 0xA1
    .word BootFileBlocksLo
    .byte 0x66, 0xC1, 0xE0, 0x03              # shl eax, 3
    .byte 0x66, 0xA3
    .word KernelSectorsLeft
    .byte 0x66, 0xB8
    .long KernelPhysBase
    .byte 0x66, 0xA3
    .word KernelDest

LoadKernelChunk:
    .byte 0x66, 0xA1
    .word KernelSectorsLeft
    .byte 0x66, 0x09, 0xC0                    # or eax, eax
    jz LoadKernelDone
    .byte 0x66, 0x3D
    .long ChunkSectors
    jbe LoadKernelUseRemainder
    .byte 0x66, 0xB8
    .long ChunkSectors
LoadKernelUseRemainder:
    mov word ptr [LoadDapCount], ax
    mov word ptr [LoadDapOffset], 0x0000
    mov word ptr [LoadDapSegment], KernelBufferSeg
    .byte 0x66, 0xA1
    .word KernelLba
    .byte 0x66, 0xA3
    .word LoadDapLba
    .byte 0x66, 0xC7, 0x06
    .word LoadDapLba + 4
    .long 0
    mov si, offset LoadDap
    mov ah, 0x42
    mov dl, byte ptr [BootDrive]
    int 0x13
    jc LoadKernelKError

    # Copy the chunk to 0x400000 + already-copied with INT 15h AH=0x87.
    # The BIOS moves CX words (max 16384) per call using the GDT below.
    movzx ecx, word ptr [LoadDapCount]
    .byte 0x66, 0xC1, 0xE1, 0x09              # shl ecx, 9 (sectors -> words)
LoadKernelCopyWord:
    .byte 0x66, 0x81, 0xF9
    .long 16384
    jbe LoadKernelCopyThis
    .byte 0x66, 0xBB
    .long 16384
    jmp LoadKernelCopyDo
LoadKernelCopyThis:
    .byte 0x66, 0x89, 0xCB                    # mov ebx, ecx
LoadKernelCopyDo:
    .byte 0x66, 0x51                          # push ecx
    # Source descriptor (GDT index 2): linear 0x10000 + copied-in-chunk.
    # Dest descriptor (GDT index 3): KernelDest + copied-in-chunk.
    .byte 0x66, 0xB8
    .long 0x10000
    .byte 0x66, 0x8B, 0x16
    .word KernelCopied
    .byte 0x66, 0x01, 0xD0                    # add eax, edx
    mov word ptr [MoveGdt + 16], 0xFFFF
    mov byte ptr [MoveGdt + 18], al
    mov byte ptr [MoveGdt + 19], ah
    .byte 0x66, 0xC1, 0xE8, 0x10              # shr eax, 16
    mov byte ptr [MoveGdt + 20], al
    mov byte ptr [MoveGdt + 23], ah
    .byte 0x66, 0xA1
    .word KernelDest
    .byte 0x66, 0x8B, 0x16
    .word KernelCopied
    .byte 0x66, 0x01, 0xD0                    # add eax, edx
    mov word ptr [MoveGdt + 24], 0xFFFF
    mov byte ptr [MoveGdt + 26], al
    mov byte ptr [MoveGdt + 27], ah
    .byte 0x66, 0xC1, 0xE8, 0x10              # shr eax, 16
    mov byte ptr [MoveGdt + 28], al
    mov byte ptr [MoveGdt + 31], ah
    .byte 0x66, 0x89, 0xD9                    # mov ecx, ebx
    mov ah, 0x87
    mov si, offset MoveGdt
    int 0x15
    jc LoadKernelCopyError
    test ah, ah
    jnz LoadKernelCopyError
    movzx eax, bx
    .byte 0x66, 0xC1, 0xE0, 0x01              # shl eax, 1 (words -> bytes)
    .byte 0x66, 0x01, 0x06
    .word KernelCopied
    .byte 0x66, 0x59                          # pop ecx
    .byte 0x66, 0x29, 0xD9                    # sub ecx, ebx
    jnz LoadKernelCopyWord

    movzx eax, word ptr [LoadDapCount]
    .byte 0x66, 0x01, 0x06
    .word KernelLba
    movzx eax, word ptr [LoadDapCount]
    .byte 0x66, 0xC1, 0xE0, 0x09              # shl eax, 9 (sectors -> bytes)
    .byte 0x66, 0x01, 0x06
    .word KernelDest
    .byte 0x66, 0xC7, 0x06
    .word KernelCopied
    .long 0
    .byte 0x66, 0xA1
    .word KernelSectorsLeft
    movzx edx, word ptr [LoadDapCount]
    .byte 0x66, 0x29, 0xD0                    # sub eax, edx
    .byte 0x66, 0xA3
    .word KernelSectorsLeft
    jmp LoadKernelChunk

LoadKernelSPopFail:
    pop es
    jmp LoadKernelSError
LoadKernelSPopFailK:
    pop es
    jmp LoadKernelKError
LoadKernelCopyError:
    .byte 0x66, 0x59                          # pop ecx (undo push of this try)
    jmp LoadKernelKError
LoadKernelDone:
    ret

# Print one character with INT 10h AH=0x0E, then halt forever.
LoadKernelPrintHalt:
    mov ah, 0x0E
    int 0x10
LoadKernelHalt:
    hlt
    jmp LoadKernelHalt
LoadKernelSError:
    mov al, 'S'
    jmp LoadKernelPrintHalt
LoadKernelKError:
    mov al, 'K'
    jmp LoadKernelPrintHalt

# INT 13h AH=0x42 disk address packet: 1-64 sectors to 0x10000.
LoadDap:
    .byte 0x10
    .byte 0x00
LoadDapCount:
    .word 0x0001
LoadDapOffset:
    .word 0x0000
LoadDapSegment:
    .word KernelBufferSeg
LoadDapLba:
    .quad 0

# INT 15h AH=0x87 move GDT: null, unused, source, dest, code, stack.
MoveGdt:
    .quad 0x0000000000000000
    .quad 0x0000000000000000
    .word 0xFFFF
    .word 0x0000
    .byte 0x00, 0x93, 0xCF, 0x00
    .word 0xFFFF
    .word 0x0000
    .byte 0x00, 0x93, 0xCF, 0x00
    .quad 0x0000000000000000
    .quad 0x0000000000000000

.global BootDrive
BootDrive:
    .byte 0x00
BootFileBlockLo:
    .long 0
BootFileBlocksLo:
    .long 0
KernelLba:
    .long 0
KernelSectorsLeft:
    .long 0
KernelDest:
    .long KernelPhysBase
KernelCopied:
    .long 0

.section .data, "aw"
.global KernelFileSize
KernelFileSize:
    .long 0

.section .text, "ax"

EnterProtectedMode:
    lgdt [GdtDescriptor]
    mov eax, cr0
    or al, 0x01
    mov cr0, eax
    ljmp 0x08, ProtectedModeEntry    # 32-bit code segment

.code32
ProtectedModeEntry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000

    call EnterLongMode

EnterLongMode:
    # check CPUID via EFLAGS.ID
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 0x00200000
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    jz NoLongMode

    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb NoLongMode
    mov eax, 0x80000001
    cpuid
    test edx, 0x20000000    # bit 29 = long mode
    jz NoLongMode

    # Temporary identity map for executing Stage2Main in long mode. After
    # loading the EXF, C builds a fresh PML4 in 0x10000..0x7FFFF with the
    # identity map, the higher-half kernel alias and the RAM direct map.
    mov edi, 0x1000
    xor eax, eax
    mov ecx, 0x0C00         # three pages, in dwords
    cld
    rep stosd

    # identity map first 1 GB with 2 MB pages
    mov dword ptr [0x1000], 0x2003   # PML4[0] -> PDPT
    mov dword ptr [0x2000], 0x3003   # PDPT[0] -> PD

    mov edi, 0x3000
    mov eax, 0x00000083              # present | writable | 2 MB
    mov ecx, 512
FillPd:
    mov [edi], eax
    add eax, 0x00200000
    add edi, 8
    loop FillPd

    mov eax, cr4
    or eax, 0x20            # PAE
    mov cr4, eax

    mov eax, 0x1000         # PML4
    mov cr3, eax

    mov ecx, 0xC0000080     # EFER
    rdmsr
    or eax, 0x00000100      # LME
    wrmsr

    mov eax, cr0
    or eax, 0x80000000      # PG
    mov cr0, eax

    ljmp 0x18, LongModeEntry

NoLongMode:
    hlt
    jmp NoLongMode

.code64
LongModeEntry:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, 0x90000

    # Stage2Main loads segments at their physical addresses, switches CR3
    # to the new PML4 and builds BootInfo at 0x4000. RAX is the EXF Entry,
    # already virtual: jump without subtracting the higher-half base.
    call Stage2Main
    mov rsp, 0x90000
    push 0                    # SysV function entry: RSP is 8 mod 16
    mov rdi, 0x4000            # BootInfoPhys (see include/bootinfo.h)
    jmp rax

.section .rodata, "a"
.align 8
Gdt:
    .quad 0x0000000000000000        # null
    .quad 0x00CF9A000000FFFF        # code 32 (0x08)
    .quad 0x00CF92000000FFFF        # data 32 (0x10)
    .quad 0x00AF9A000000FFFF        # code 64 (0x18)
GdtEnd:

GdtDescriptor:
    .word GdtEnd - Gdt - 1
    .long Gdt
