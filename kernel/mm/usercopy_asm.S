/* SPDX-License-Identifier: BSD-3-Clause */
/* Raw user-memory primitives with SMAP bracketing and .extable fixups.
 *
 * System V AMD64:
 *   CopyFromUserRaw(void *Dst, const void *Src, uint64_t Len)
 *     rdi = Dst (kernel), rsi = Src (user), rdx = Len.
 *     Returns bytes NOT copied: 0 on success, >0 when the fixup ran.
 *   CopyToUserRaw(void *Dst, const void *Src, uint64_t Len)
 *     rdi = Dst (user), rsi = Src (kernel), rdx = Len. Same return.
 *   StrnlenUserRaw(const char *Src, uint64_t Max)
 *     rdi = Src (user), rsi = Max. Returns length 0..Max, or -1 on fault.
 *
 * Each instruction touching user memory carries a label; its fixup clears
 * AC (clac) so SMAP is re-enabled even on the fault path. The (address,
 * fixup) pairs live in .extable for ExtableFind() in usercopy.c, which the
 * future #PF handler uses to redirect RIP to the fixup.
 *
 * Public checked wrappers CopyFromUser/CopyToUser/StrnlenUser live in
 * usercopy.c (range check + ErrFault translation); the Raw names here keep
 * the required CopyFromUser/CopyToUser/StrnlenUser substrings while avoiding
 * duplicate symbols at link time. File is usercopy_asm.S (not usercopy.S)
 * so its object build/kernel/mm/usercopy_asm.o does not collide with the
 * C wrapper object build/kernel/mm/usercopy.o.
 */
.intel_syntax noprefix
.section .text

/* uint64_t CopyFromUserRaw(void *Dst, const void *Src, uint64_t Len); */
.global CopyFromUserRaw
.type CopyFromUserRaw, @function
CopyFromUserRaw:
    test rdx, rdx
    jz .Lcfu_zero
    mov rcx, rdx
    stac
    cld
.Lcfu_mem:
    rep movsb
    clac
    xor eax, eax
    ret
.Lcfu_zero:
    xor eax, eax
    ret
.Lcfu_fixup:
    clac
    mov rax, rcx
    ret
.size CopyFromUserRaw, .-CopyFromUserRaw
.pushsection .extable, "a", @progbits
.balign 8
.quad .Lcfu_mem
.quad .Lcfu_fixup
.popsection

/* uint64_t CopyToUserRaw(void *Dst, const void *Src, uint64_t Len); */
.global CopyToUserRaw
.type CopyToUserRaw, @function
CopyToUserRaw:
    test rdx, rdx
    jz .Lctu_zero
    mov rcx, rdx
    stac
    cld
.Lctu_mem:
    rep movsb
    clac
    xor eax, eax
    ret
.Lctu_zero:
    xor eax, eax
    ret
.Lctu_fixup:
    clac
    mov rax, rcx
    ret
.size CopyToUserRaw, .-CopyToUserRaw
.pushsection .extable, "a", @progbits
.balign 8
.quad .Lctu_mem
.quad .Lctu_fixup
.popsection

/* int64_t StrnlenUserRaw(const char *Src, uint64_t Max); */
.global StrnlenUserRaw
.type StrnlenUserRaw, @function
StrnlenUserRaw:
    test rsi, rsi
    jz .Lslu_zero
    mov rcx, rsi
    xor eax, eax
    stac
    cld
.Lslu_mem:
    repne scasb
    clac
    jz .Lslu_found
    /* Not found: NUL not in first Max bytes, length is Max. */
    mov rax, rsi
    sub rax, rcx
    ret
.Lslu_found:
    /* Found: rax = Max - remaining - 1 (drop the NUL itself). */
    mov rax, rsi
    sub rax, rcx
    dec rax
    ret
.Lslu_zero:
    xor eax, eax
    ret
.Lslu_fixup:
    clac
    mov rax, -1
    ret
.size StrnlenUserRaw, .-StrnlenUserRaw
.pushsection .extable, "a", @progbits
.balign 8
.quad .Lslu_mem
.quad .Lslu_fixup
.popsection
