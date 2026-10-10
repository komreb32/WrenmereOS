/* SPDX-License-Identifier: BSD-3-Clause */
.intel_syntax noprefix
.section .text

/*
 * void EnterUserMode(uint64_t Entry, uint64_t UserRsp,
 *                    uint64_t Arg0, uint64_t Arg1);
 *
 * System V AMD64: rdi = Entry, rsi = UserRsp, rdx = Arg0, rcx = Arg1.
 *
 * Drops to ring 3 and starts executing user code at Entry. Does not return.
 * The iretq frame is built on the current kernel stack (iretq loads RSP from
 * the frame itself, so the user still starts with rsp = UserRsp):
 *
 *   [rsp+0 ] RIP     = Entry
 *   [rsp+8 ] CS      = 0x23 (SelUserCode)
 *   [rsp+16] RFLAGS  = 0x202 (IF set, reserved bit 1 set)
 *   [rsp+24] RSP     = UserRsp
 *   [rsp+32] SS      = 0x1B (SelUserData)
 *
 * User registers: rdi = Arg0, rsi = Arg1, every other GPR = 0.
 *
 * The TSS is not touched: the caller must have loaded TSS.Rsp0 with the
 * kernel stack for the current thread before calling this function.
 *
 * Per docs/abi.md the kernel GS base (PerCpu) is active on entry and the user
 * GS value lives in IA32_KERNEL_GS_BASE, so swapgs once just before iretq.
 */
.global EnterUserMode
.type EnterUserMode, @function
EnterUserMode:
    /* Build the iretq frame; the last push lands at [rsp] = RIP. */
    push 0x1B                   /* SS      = SelUserData */
    push rsi                    /* RSP     = UserRsp */
    push 0x202                  /* RFLAGS  = IF | reserved bit 1 */
    push 0x23                   /* CS      = SelUserCode */
    push rdi                    /* RIP     = Entry */

    /* User argument registers. */
    mov rdi, rdx                /* rdi = Arg0 */
    mov rsi, rcx                /* rsi = Arg1 */

    /* Every other general purpose register starts at zero. */
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor ebp, ebp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    /* Switch to the user GS base and iretq into ring 3. Never returns. */
    swapgs
    iretq
.size EnterUserMode, .-EnterUserMode
