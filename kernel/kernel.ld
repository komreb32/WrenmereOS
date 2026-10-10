/* SPDX-License-Identifier: BSD-3-Clause */
/* Kernel linker script: higher-half ELF64, physical base 0x100000. */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(KernelEntry)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    KernelVirtBase = 0xFFFFFFFF80000000;
    . = KernelVirtBase + 0x100000;

    KernelStart = .;

    /* Stage2 image location (bootloader, not part of the kernel ELF). */
    Stage2Start = 0x8000;
    Stage2End = 0xC000;

    .text : AT(ADDR(.text) - KernelVirtBase) {
        *(.text)
        *(.text.*)
    } :text

    .rodata : AT(ADDR(.rodata) - KernelVirtBase) {
        *(.rodata)
        *(.rodata.*)
        /* Fault-recovery table for usercopy.S: (fault RIP, fixup RIP)
         * pairs, 8-byte aligned. SORT keeps ExtableFind()'s binary search
         * valid no matter which object contributes entries. */
        . = ALIGN(8);
        ExtableStart = .;
        KEEP(*(SORT(.extable)))
        KEEP(*(SORT(.extable.*)))
        ExtableEnd = .;
    } :rodata

    .data : AT(ADDR(.data) - KernelVirtBase) {
        *(.data)
        *(.data.*)
    } :data

    .bss : AT(ADDR(.bss) - KernelVirtBase) {
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(0x1000);
        KernelBootStackBottom = .;
        . += 0x4000; /* 16 KiB; the stack grows down from the aligned top. */
        KernelBootStackTop = .;
    } :data

    KernelEnd = .;
    KernelPhysStart = LOADADDR(.text);
    KernelPhysEnd = LOADADDR(.bss) + SIZEOF(.bss);

    ASSERT(KernelBootStackTop - KernelBootStackBottom == 0x4000,
           "kernel bootstrap stack must be 16 KiB")
    ASSERT((KernelBootStackTop & 0xF) == 0,
           "kernel bootstrap stack must be 16-byte aligned")

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.eh_frame.*)
        *(.note*)
        *(.init_array)
        *(.init_array.*)
        *(.fini_array)
        *(.fini_array.*)
    }
}
