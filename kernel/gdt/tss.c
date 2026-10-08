/* SPDX-License-Identifier: BSD-3-Clause */
#include <kernel/gdt/gdt.h>

#define IstStackSize 16384

static Tss KernelTss;

/* Three 16 KiB IST stacks aligned to 16 in .bss */
static uint8_t Ist1Stack[IstStackSize] __attribute__((aligned(16)));
static uint8_t Ist2Stack[IstStackSize] __attribute__((aligned(16)));
static uint8_t Ist3Stack[IstStackSize] __attribute__((aligned(16)));

void TssInit(void)
{
    uint8_t *ptr = (uint8_t *)&KernelTss;
    for (uint32_t i = 0; i < sizeof(KernelTss); i++)
        ptr[i] = 0;

    /* IST1 for #DF (Double Fault, vector 8) */
    KernelTss.Ist1 = (uint64_t)(uintptr_t)(Ist1Stack + IstStackSize);

    /* IST2 for NMI (Non-Maskable Interrupt, vector 2) */
    KernelTss.Ist2 = (uint64_t)(uintptr_t)(Ist2Stack + IstStackSize);

    /* IST3 for #MC (Machine Check, vector 18) */
    KernelTss.Ist3 = (uint64_t)(uintptr_t)(Ist3Stack + IstStackSize);

    /* No I/O permissions bitmap (IopbOffset >= TSS limit) */
    KernelTss.IopbOffset = (uint16_t)sizeof(KernelTss);

    /* Fill TSS descriptor in GDT at SelTss (0x28) */
    GdtSetTss((uint64_t)(uintptr_t)&KernelTss, (uint16_t)(sizeof(KernelTss) - 1));

    /* Load Task Register */
    __asm__ volatile ("ltr %0" : : "r"((uint16_t)SelTss));
}

void TssSetRsp0(uint64_t rsp0)
{
    KernelTss.Rsp0 = rsp0;
}

