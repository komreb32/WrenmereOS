/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef CPU_H
#define CPU_H

#include <lib/stdint.h>

/* MSR addresses */
#define MsrEfer             0xC0000080u
#define MsrStar             0xC0000081u
#define MsrLstar            0xC0000082u
#define MsrSfmask           0xC0000084u
#define MsrFsBase           0xC0000100u
#define MsrGsBase           0xC0000101u
#define MsrKernelGsBase     0xC0000102u

#define MSR_EFER            MsrEfer
#define MSR_STAR            MsrStar
#define MSR_LSTAR           MsrLstar
#define MSR_SFMASK          MsrSfmask
#define MSR_FS_BASE         MsrFsBase
#define MSR_GS_BASE         MsrGsBase
#define MSR_KERNEL_GS_BASE  MsrKernelGsBase

/* EFER bits */
#define EferSce             (1ULL << 0)   /* System Call Extensions */
#define EferLme             (1ULL << 8)   /* Long Mode Enable */
#define EferLma             (1ULL << 10)  /* Long Mode Active */
#define EferNxe             (1ULL << 11)  /* No-Execute Enable */
#define EferSvm             (1ULL << 12)  /* Secure Virtual Machine Enable */
#define EferFfxsr           (1ULL << 14)  /* Fast FXSAVE/FXRSTOR */

#define EFER_SCE            EferSce
#define EFER_LME            EferLme
#define EFER_LMA            EferLma
#define EFER_NXE            EferNxe
#define EFER_SVM            EferSvm
#define EFER_FFXSR          EferFfxsr

/* CR0 bits */
#define Cr0Pe               (1ULL << 0)   /* Protection Enable */
#define Cr0Mp               (1ULL << 1)   /* Monitor Coprocessor */
#define Cr0Em               (1ULL << 2)   /* Emulation */
#define Cr0Ts               (1ULL << 3)   /* Task Switched */
#define Cr0Et               (1ULL << 4)   /* Extension Type */
#define Cr0Ne               (1ULL << 5)   /* Numeric Error */
#define Cr0Wp               (1ULL << 16)  /* Write Protect */
#define Cr0Am               (1ULL << 18)  /* Alignment Mask */
#define Cr0Nw               (1ULL << 29)  /* Not Write-through */
#define Cr0Cd               (1ULL << 30)  /* Cache Disable */
#define Cr0Pg               (1ULL << 31)  /* Paging */

#define CR0_PE              Cr0Pe
#define CR0_MP              Cr0Mp
#define CR0_EM              Cr0Em
#define CR0_TS              Cr0Ts
#define CR0_ET              Cr0Et
#define CR0_NE              Cr0Ne
#define CR0_WP              Cr0Wp
#define CR0_AM              Cr0Am
#define CR0_NW              Cr0Nw
#define CR0_CD              Cr0Cd
#define CR0_PG              Cr0Pg

/* CR4 bits */
#define Cr4Vme              (1ULL << 0)   /* Virtual-8086 Mode Extensions */
#define Cr4Pvi              (1ULL << 1)   /* Protected-Mode Virtual Interrupts */
#define Cr4Tsd              (1ULL << 2)   /* Time Stamp Disable */
#define Cr4De               (1ULL << 3)   /* Debugging Extensions */
#define Cr4Pse              (1ULL << 4)   /* Page Size Extension */
#define Cr4Pae              (1ULL << 5)   /* Physical Address Extension */
#define Cr4Mce              (1ULL << 6)   /* Machine Check Enable */
#define Cr4Pge              (1ULL << 7)   /* Page Global Enable */
#define Cr4Pce              (1ULL << 8)   /* Performance-Monitoring Counter Enable */
#define Cr4Osfxsr           (1ULL << 9)   /* OS Support for FXSAVE and FXRSTOR */
#define Cr4Osxmmexcpt       (1ULL << 10)  /* OS Support for SIMD FP Exceptions */
#define Cr4Umip             (1ULL << 11)  /* User-Mode Instruction Prevention */
#define Cr4La57             (1ULL << 12)  /* 57-bit linear addresses */
#define Cr4Vmxe             (1ULL << 13)  /* VMX Enable */
#define Cr4Smxe             (1ULL << 14)  /* SMX Enable */
#define Cr4Fsgsbase         (1ULL << 16)  /* FSGSBASE Enable */
#define Cr4Pcide            (1ULL << 17)  /* PCID Enable */
#define Cr4Osxsave          (1ULL << 18)  /* XSAVE and Processor Extended States-Enable */
#define Cr4Smep             (1ULL << 20)  /* Supervisor Mode Execution Prevention */
#define Cr4Smap             (1ULL << 21)  /* Supervisor Mode Access Prevention */
#define Cr4Pke              (1ULL << 22)  /* Protection Keys Enable */
#define Cr4Cet              (1ULL << 23)  /* Control-flow Enforcement Technology */

#define CR4_VME             Cr4Vme
#define CR4_PVI             Cr4Pvi
#define CR4_TSD             Cr4Tsd
#define CR4_DE              Cr4De
#define CR4_PSE             Cr4Pse
#define CR4_PAE             Cr4Pae
#define CR4_MCE             Cr4Mce
#define CR4_PGE             Cr4Pge
#define CR4_PCE             Cr4Pce
#define CR4_OSFXSR          Cr4Osfxsr
#define CR4_OSXMMEXCPT      Cr4Osxmmexcpt
#define CR4_UMIP            Cr4Umip
#define CR4_LA57            Cr4La57
#define CR4_VMXE            Cr4Vmxe
#define CR4_SMXE            Cr4Smxe
#define CR4_FSGSBASE        Cr4Fsgsbase
#define CR4_PCIDE           Cr4Pcide
#define CR4_OSXSAVE         Cr4Osxsave
#define CR4_SMEP            Cr4Smep
#define CR4_SMAP            Cr4Smap
#define CR4_PKE             Cr4Pke
#define CR4_CET             Cr4Cet

static inline uint64_t ReadMsr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void WriteMsr(uint32_t msr, uint64_t val)
{
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

static inline uint64_t ReadCr0(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline uint64_t ReadCr2(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint64_t ReadCr3(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline uint64_t ReadCr4(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline void WriteCr0(uint64_t val)
{
    __asm__ volatile ("mov %0, %%cr0" : : "r"(val) : "memory");
}

static inline void WriteCr3(uint64_t val)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(val) : "memory");
}

static inline void WriteCr4(uint64_t val)
{
    __asm__ volatile ("mov %0, %%cr4" : : "r"(val) : "memory");
}

static inline void Cpuid(uint32_t leaf, uint32_t sub, uint32_t regs[4])
{
    __asm__ volatile ("cpuid"
                      : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                      : "a"(leaf), "c"(sub));
}

static inline void InvalidatePage(uint64_t addr)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline uint64_t ReadTsc(void)
{
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static inline void IrqEnable(void)
{
    __asm__ volatile ("sti" : : : "memory");
}

static inline void IrqDisable(void)
{
    __asm__ volatile ("cli" : : : "memory");
}

static inline uint64_t ReadFlags(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    return flags;
}

struct CpuFeatures {
    uint32_t Nx;
    uint32_t Smep;
    uint32_t Smap;
    uint32_t Umip;
    uint32_t Syscall;
    uint32_t Fxsr;
};

extern struct CpuFeatures CpuFeatures;

void CpuFeaturesDetect(void);
void CpuFeaturesEnable(void);

/* Switch to ring 3 and start executing user code at Entry. Does not return. */
void EnterUserMode(uint64_t Entry, uint64_t UserRsp, uint64_t Arg0, uint64_t Arg1);

#endif

