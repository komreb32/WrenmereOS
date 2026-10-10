/* SPDX-License-Identifier: BSD-3-Clause */
// Kernel interrupt handling: IDT, exceptions and the 8259 PIC.
#ifndef KERNEL_IDT_H
#define KERNEL_IDT_H

#include <lib/stdint.h>

#define IdtTypeInterrupt 0x0E
#define IdtTypeTrap      0x0F

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
void IdtSetGate(int vector, void *handler, uint8_t ist, uint8_t dpl, uint8_t tipo);
void IdtSetUserGate(int vector, void *handler);

#endif
