OUTPUT_FORMAT(binary)
OUTPUT_ARCH(i386:x86-64)
ENTRY(Stage2Start)

SECTIONS
{
    . = 0x8000;

    .text.entry : {
        *(.text.entry)
    }

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        BssStart = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        BssEnd = .;
    }

    # First free byte after Stage2
    Stage2End = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
    }
}
