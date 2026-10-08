/* SPDX-License-Identifier: BSD-3-Clause */
.section .text

# no error code stubs
.macro IsrNoErr vector
.global IsrStub\vector
IsrStub\vector:
    pushq $0
    pushq $\vector
    jmp IsrCommon
.endm

# hardware error code stubs
.macro IsrErr vector
.global IsrStub\vector
IsrStub\vector:
    pushq $\vector
    jmp IsrCommon
.endm

# shared interrupt handler, the pushes must match InterruptFrame
IsrCommon:
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %rbx
    pushq %rax

    cld

    # pass the frame pointer as the only argument
    movq %rsp, %rdi
    call IrqDispatch

    popq %rax
    popq %rbx
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rbp
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15

    # drop the vector and the error code
    addq $16, %rsp
    iretq

# cpu exceptions (0-31); in 64-bit mode 21, 29 and 30 push an error code too
IsrNoErr 0
IsrNoErr 1
IsrNoErr 2
IsrNoErr 3
IsrNoErr 4
IsrNoErr 5
IsrNoErr 6
IsrNoErr 7
IsrErr   8
IsrNoErr 9
IsrErr   10
IsrErr   11
IsrErr   12
IsrErr   13
IsrErr   14
IsrNoErr 15
IsrNoErr 16
IsrErr   17
IsrNoErr 18
IsrNoErr 19
IsrNoErr 20
IsrErr   21
IsrNoErr 22
IsrNoErr 23
IsrNoErr 24
IsrNoErr 25
IsrNoErr 26
IsrNoErr 27
IsrNoErr 28
IsrErr   29
IsrErr   30
IsrNoErr 31

# hardware irqs (32-47)
IsrNoErr 32
IsrNoErr 33
IsrNoErr 34
IsrNoErr 35
IsrNoErr 36
IsrNoErr 37
IsrNoErr 38
IsrNoErr 39
IsrNoErr 40
IsrNoErr 41
IsrNoErr 42
IsrNoErr 43
IsrNoErr 44
IsrNoErr 45
IsrNoErr 46
IsrNoErr 47
