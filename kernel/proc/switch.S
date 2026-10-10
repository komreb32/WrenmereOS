/* SPDX-License-Identifier: BSD-3-Clause */
.intel_syntax noprefix
.code64
.section .text

/*
 * void SwitchContext(Thread *From, Thread *To);
 *
 * System V AMD64: rdi = From, rsi = To.
 *
 * Save From's callee-saved registers (rbp, rbx, r12-r15) and stack pointer,
 * then restore To's and return. Both sides of the switch use the same
 * frame layout, so a plain ret transfers control correctly.
 */
.global SwitchContext
.type SwitchContext, @function
SwitchContext:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov rax, rsp
    mov [rdi + 16], rax     /* From->SavedRsp = rsp */
    mov rsp, [rsi + 16]     /* rsp = To->SavedRsp */
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
.size SwitchContext, .-SwitchContext
