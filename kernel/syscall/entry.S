/* SPDX-License-Identifier: BSD-3-Clause */
.intel_syntax noprefix
.section .text

.global SyscallEntry

# Default weak stub so linking kernel.elf succeeds until SyscallDispatch is implemented
.weak SyscallDispatch
SyscallDispatch:
    ret

# Entry point for the syscall instruction (registered in IA32_LSTAR)
SyscallEntry:
    # Switch to kernel GS base
    swapgs

    # Save user stack pointer to PerCpu.UserRspScratch (offset 16)
    mov gs:[16], rsp

    # Load kernel stack pointer from PerCpu.KernelRsp (offset 8)
    mov rsp, gs:[8]

    # Construct TrapFrame hardware/interrupt context (highest address first)
    push 0x1B                # Ss = 0x1B (SelUserData)
    push qword ptr gs:[16]   # Rsp = UserRspScratch
    push r11                 # Rflags = r11 (saved user RFLAGS from syscall)
    push 0x23                # Cs = 0x23 (SelUserCode)
    push rcx                 # Rip = rcx (saved user RIP from syscall)
    push 0                   # ErrorCode = 0
    push 0x80                # Vector = 0x80

    # Save general purpose registers in TrapFrame order (lowest address R15 .. Rax)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    cld

    # Pass pointer to TrapFrame as first argument (RDI)
    mov rdi, rsp
    call SyscallDispatch

    # Restore registers from TrapFrame (rax gets whatever dispatcher left in frame->Rax)
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    # Drop Vector and ErrorCode
    add rsp, 16

    # Return to ring 3: restore user GS and return via iretq
    swapgs
    iretq

