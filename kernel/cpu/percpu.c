/* SPDX-License-Identifier: BSD-3-Clause */
#include <cpu.h>
#include <percpu.h>

/* Static PerCpu instance for CPU 0 (BSP) */
static struct PerCpu BspCpu;

void PerCpuInit(void)
{
    BspCpu.Self = (uint64_t)(uintptr_t)&BspCpu;
    BspCpu.KernelRsp = 0;
    BspCpu.UserRspScratch = 0;
    BspCpu.CurrentThread = 0;
    BspCpu.CpuId = 0;
    BspCpu.Flags = 0;

    WriteMsr(MsrGsBase, (uint64_t)(uintptr_t)&BspCpu);
    WriteMsr(MsrKernelGsBase, 0);
}

struct PerCpu *PerCpuGet(void)
{
    struct PerCpu *cpu;
    __asm__ volatile ("movq %%gs:0, %0" : "=r"(cpu));
    return cpu;
}

void PerCpuSetKernelRsp(uint64_t rsp)
{
    __asm__ volatile ("movq %0, %%gs:8" : : "r"(rsp) : "memory");
}

