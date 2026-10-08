/* SPDX-License-Identifier: BSD-3-Clause */
#include <lib/stdint.h>
#include <kernel/gdt/gdt.h>

typedef struct {
    uint16_t LimitLow;
    uint16_t BaseLow;
    uint8_t  BaseMid;
    uint8_t  Access;
    uint8_t  Flags;
    uint8_t  BaseHigh;
} GdtEntry;

typedef struct {
    uint16_t Limit;
    uint64_t Base;
} __attribute__((packed)) GdtDescriptor;

static const GdtEntry Gdt[] = {
    { 0,      0, 0, 0x00, 0x00, 0 }, // null
    { 0xFFFF, 0, 0, 0x9A, 0xAF, 0 }, // kernel code 64 (0x08)
    { 0xFFFF, 0, 0, 0x92, 0xCF, 0 }, // kernel data (0x10)
};

// load the table, reload the data segments and CS
int GdtInit(void)
{
    GdtDescriptor descriptor;

    descriptor.Limit = (uint16_t)(sizeof(Gdt) - 1);
    descriptor.Base = (uint64_t)(uintptr_t)Gdt;

    __asm__ volatile (
        "lgdt %[descriptor]\n\t"
        "movw $%c[data], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "pushq $%c[code]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :
        : [descriptor] "m" (descriptor),
          [code] "i" (KernelCodeSelector),
          [data] "i" (KernelDataSelector)
        : "rax", "memory"
    );

    return 0;
}
