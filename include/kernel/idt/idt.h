/* SPDX-License-Identifier: BSD-3-Clause */
// Kernel interrupt handling: IDT, exceptions and the 8259 PIC.
#ifndef KERNEL_IDT_H
#define KERNEL_IDT_H

#include <lib/stdint.h>

// registers pushed by the isr stubs, in stack order
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t Vector, Error;
    uint64_t rip, cs, rflags, rsp, ss;
} InterruptFrame;

typedef void (*IrqHandler)(InterruptFrame *frame);

void IdtInit(void);
int IdtStart(void);
int PicInit(void);
void IrqRegister(int irq, IrqHandler handler);
void IrqUnregister(int irq);
uint32_t IrqSpuriousCount(int irq);
void IrqEnable(void);
void IrqDisable(void);

#endif
