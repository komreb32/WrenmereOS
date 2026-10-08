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
    uint16_t LimitLow;
    uint16_t BaseLow;
    uint8_t  BaseMid;
    uint8_t  Access;
    uint8_t  Flags;
    uint8_t  BaseHigh;
    uint32_t BaseUpper;
    uint32_t Reserved;
} __attribute__((packed)) TssDescriptor;

typedef struct {
    uint16_t Limit;
    uint64_t Base;
} __attribute__((packed)) GdtDescriptor;

/*
 * Kernel GDT:
 *   0x00: null descriptor
 *   0x08: KernelCode (64-bit, DPL 0)
 *   0x10: KernelData (DPL 0)
 *   0x18: UserData   (DPL 3, SelUserData = 0x1B)
 *   0x20: UserCode   (64-bit, DPL 3, SelUserCode = 0x23)
 *   0x28: TSS descriptor (16 bytes, SelTss = 0x28)
 */
static GdtEntry Gdt[7] = {
    { 0,      0, 0, 0x00, 0x00, 0 }, // 0x00: null
    { 0xFFFF, 0, 0, 0x9A, 0xAF, 0 }, // 0x08: KernelCode 64-bit (DPL 0)
    { 0xFFFF, 0, 0, 0x92, 0xCF, 0 }, // 0x10: KernelData (DPL 0)
    { 0xFFFF, 0, 0, 0xF2, 0xCF, 0 }, // 0x18: UserData (DPL 3)
    { 0xFFFF, 0, 0, 0xFA, 0xAF, 0 }, // 0x20: UserCode 64-bit (DPL 3)
    { 0,      0, 0, 0x89, 0x00, 0 }, // 0x28: TSS descriptor low
    { 0,      0, 0, 0x00, 0x00, 0 }, // 0x30: TSS descriptor high
};

void GdtSetTss(uint64_t base, uint16_t limit)
{
    TssDescriptor *tssDesc = (TssDescriptor *)&Gdt[5];

    tssDesc->LimitLow  = limit;
    tssDesc->BaseLow   = (uint16_t)(base & 0xFFFF);
    tssDesc->BaseMid   = (uint8_t)((base >> 16) & 0xFF);
    tssDesc->Access    = 0x89; // Present, DPL 0, 64-bit Available TSS
    tssDesc->Flags     = 0x00; // Byte granular, limit 19:16 = 0
    tssDesc->BaseHigh  = (uint8_t)((base >> 24) & 0xFF);
    tssDesc->BaseUpper = (uint32_t)(base >> 32);
    tssDesc->Reserved  = 0;
}

// load the table, reload the data segments, CS, set FS/GS to 0, and init TSS
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
        "xorw %%ax, %%ax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "pushq $%c[code]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :
        : [descriptor] "m" (descriptor),
          [code] "i" (SelKernelCode),
          [data] "i" (SelKernelData)
        : "rax", "memory"
    );

    TssInit();

    return 0;
}
