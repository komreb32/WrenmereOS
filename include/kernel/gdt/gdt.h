/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef KERNEL_GDT_H
#define KERNEL_GDT_H

#include <lib/stdint.h>

#define SelKernelCode      0x08
#define SelKernelData      0x10
#define SelUserData        0x1B
#define SelUserCode        0x23
#define SelTss             0x28

#define KernelCodeSelector SelKernelCode
#define KernelDataSelector SelKernelData

typedef struct __attribute__((packed)) Tss {
    uint32_t Reserved0;
    uint64_t Rsp0;
    uint64_t Rsp1;
    uint64_t Rsp2;
    uint64_t Reserved1;
    uint64_t Ist1;
    uint64_t Ist2;
    uint64_t Ist3;
    uint64_t Ist4;
    uint64_t Ist5;
    uint64_t Ist6;
    uint64_t Ist7;
    uint64_t Reserved2;
    uint16_t Reserved3;
    uint16_t IopbOffset;
} Tss;

int GdtInit(void);
void GdtSetTss(uint64_t base, uint16_t limit);

void TssInit(void);
void TssSetRsp0(uint64_t rsp0);

#endif
