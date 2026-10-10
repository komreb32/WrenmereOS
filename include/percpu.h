/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef PERCPU_H
#define PERCPU_H

#include <lib/stdint.h>

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#ifndef static_assert
#define static_assert _Static_assert
#endif

typedef struct __attribute__((packed)) PerCpu {
    uint64_t Self;             /* Offset  0: pointer to this PerCpu struct */
    uint64_t KernelRsp;        /* Offset  8: top of the current thread kernel stack */
    uint64_t UserRspScratch;   /* Offset 16: scratch space for user RSP */
    uint64_t CurrentThread;    /* Offset 24: pointer to current thread */
    uint32_t CpuId;            /* Offset 32: CPU identifier */
    uint32_t Flags;            /* Offset 36: CPU flags */
} PerCpu;

static_assert(offsetof(struct PerCpu, Self) == 0, "PerCpu.Self offset must be 0");
static_assert(offsetof(struct PerCpu, KernelRsp) == 8, "PerCpu.KernelRsp offset must be 8");
static_assert(offsetof(struct PerCpu, UserRspScratch) == 16, "PerCpu.UserRspScratch offset must be 16");
static_assert(offsetof(struct PerCpu, CurrentThread) == 24, "PerCpu.CurrentThread offset must be 24");
static_assert(offsetof(struct PerCpu, CpuId) == 32, "PerCpu.CpuId offset must be 32");
static_assert(offsetof(struct PerCpu, Flags) == 36, "PerCpu.Flags offset must be 36");
static_assert(sizeof(struct PerCpu) == 40, "PerCpu size must be 40 bytes");

void PerCpuInit(void);
struct PerCpu *PerCpuGet(void);
void PerCpuSetKernelRsp(uint64_t rsp);

#endif

