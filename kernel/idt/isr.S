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

# shared interrupt handler, matching TrapFrame layout from docs/abi.md
IsrCommon:
    # Check saved CS: if RPL 3, swapgs before touching GS
    testb $3, 24(%rsp)
    jz 1f
    swapgs
1:
    # Save registers in TrapFrame order (lowest address R15 .. Rax)
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld

    # pass the frame pointer as the only argument
    movq %rsp, %rdi
    call IrqDispatch

    # Restore registers in reverse order
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    # drop the vector and the error code
    addq $16, %rsp

    # Check saved CS right before iretq: if RPL 3, swapgs back to user GS
    testb $3, 8(%rsp)
    jz 2f
    swapgs
2:
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
