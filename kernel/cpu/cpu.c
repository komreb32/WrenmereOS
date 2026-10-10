/* SPDX-License-Identifier: BSD-3-Clause */
#include <cpu.h>
#include <lib/stdio.h>

struct CpuFeatures CpuFeatures = {0};

void CpuFeaturesDetect(void)
{
    uint32_t regs[4];

    /* Basic CPUID information */
    Cpuid(0, 0, regs);
    uint32_t maxBasicLeaf = regs[0];

    /* Extended CPUID information */
    Cpuid(0x80000000u, 0, regs);
    uint32_t maxExtLeaf = regs[0];

    /* Leaf 1: EDX bit 24 is FXSR */
    if (maxBasicLeaf >= 1) {
        Cpuid(1, 0, regs);
        if (regs[3] & (1u << 24))
            CpuFeatures.Fxsr = 1;
    }

    /* Leaf 7, Subleaf 0: EBX bit 7 = SMEP, EBX bit 20 = SMAP, ECX bit 2 = UMIP */
    if (maxBasicLeaf >= 7) {
        Cpuid(7, 0, regs);
        if (regs[1] & (1u << 7))
            CpuFeatures.Smep = 1;
        if (regs[1] & (1u << 20))
            CpuFeatures.Smap = 1;
        if (regs[2] & (1u << 2))
            CpuFeatures.Umip = 1;
    }

    /* Leaf 0x80000001: EDX bit 11 = SYSCALL, EDX bit 20 = NX */
    if (maxExtLeaf >= 0x80000001u) {
        Cpuid(0x80000001u, 0, regs);
        if (regs[3] & (1u << 11))
            CpuFeatures.Syscall = 1;
        if (regs[3] & (1u << 20))
            CpuFeatures.Nx = 1;
    }
}

static void CpuFeaturesEnableImpl(void)
{
    CpuFeaturesDetect();

    /* Enable EFER.SCE and NXE */
    uint64_t efer = ReadMsr(MsrEfer);
    if (CpuFeatures.Syscall)
        efer |= EferSce;
    if (CpuFeatures.Nx)
        efer |= EferNxe;
    WriteMsr(MsrEfer, efer);

    /* Enable CR0.WP */
    uint64_t cr0 = ReadCr0();
    cr0 |= Cr0Wp;
    WriteCr0(cr0);

    /* Enable CR4 SMEP, SMAP, and UMIP if available */
    uint64_t cr4 = ReadCr4();
    if (CpuFeatures.Smep)
        cr4 |= Cr4Smep;
    if (CpuFeatures.Smap)
        cr4 |= Cr4Smap;
    if (CpuFeatures.Umip)
        cr4 |= Cr4Umip;
    WriteCr4(cr4);

    /* Read back configuration to verify and report active features */
    uint64_t curCr0 = ReadCr0();
    uint64_t curCr4 = ReadCr4();
    uint64_t curEfer = ReadMsr(MsrEfer);

    KPrint("CPU: active features:");
    if (curCr0 & Cr0Wp)
        KPrint(" WP");
    if (curEfer & EferSce)
        KPrint(" SCE");
    if (curEfer & EferNxe)
        KPrint(" NX");
    if (curCr4 & Cr4Smep)
        KPrint(" SMEP");
    if (curCr4 & Cr4Smap)
        KPrint(" SMAP");
    if (curCr4 & Cr4Umip)
        KPrint(" UMIP");
    KPrintln("");
}

void CpuFeaturesEnable(void)
{
    /*
     * Preserve caller-saved registers so calling CpuFeaturesEnable
     * from naked functions (e.g., KernelEntry) does not clobber RDI (BootInfo).
     */
    __asm__ volatile (
        "pushq %%rdi\n\t"
        "pushq %%rsi\n\t"
        "pushq %%rdx\n\t"
        "pushq %%rcx\n\t"
        "pushq %%rax\n\t"
        "pushq %%r8\n\t"
        "pushq %%r9\n\t"
        "pushq %%r10\n\t"
        "pushq %%r11\n\t"
        "call %P0\n\t"
        "popq %%r11\n\t"
        "popq %%r10\n\t"
        "popq %%r9\n\t"
        "popq %%r8\n\t"
        "popq %%rax\n\t"
        "popq %%rcx\n\t"
        "popq %%rdx\n\t"
        "popq %%rsi\n\t"
        "popq %%rdi\n\t"
        :
        : "i"(CpuFeaturesEnableImpl)
        : "memory"
    );
}

