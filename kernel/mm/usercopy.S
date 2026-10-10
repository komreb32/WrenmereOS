/* SPDX-License-Identifier: BSD-3-Clause */
/* Kernel user-memory copy primitives with SMAP bracketing and .extable fixups.
 *
 * System V AMD64 calling convention:
 *   rdi = destination, rsi = source, rdx = length (64-bit).
 *
 *   uint64_t CopyFromUser(void *Dst, const void *Src, uint64_t Len);
 *     Dst is in kernel space; Src is user space. Uses rep movsb to transfer
 *     Len bytes from user to kernel, bracketed by stac/clac so SMAP is
 *     disabled during the copy and re-enabled on the fault path.
 *
 *   uint64_t CopyToUser(void *Dst, const void *Src, uint64_t Len);
 *     Dst is user space; Src is in kernel space. Same bracketing, rep movsb.
 *
 *   int64_t  StrnlenUser(const char *Src, uint64_t Max);
 *     Scans user buffer [Src, Src+Max) for the first NUL, returns count of
 *     bytes before the NUL, or -1 when the scan faults. Uses repne scasb.
 *
 * Every instruction that touches user memory carries a label and a sibling
 * fixup label that clears AC (clac) to re-enable SMAP even on the fault path.
 * The (instruction-address, fixup-address) pairs are emitted to .extable.
 * The future #PF handler (kernel/idt/pagefault.c) calls ExtableFind(rip) to
 * redirect RIP to the fixup when a usercopy primitive faults in ring 0.
 */
.intel_syntax noprefix
.section .text

/* uint64_t CopyFromUser(void *Dst, const void *Src, uint64_t Len); */
.global CopyFromUser
.type CopyFromUser, @function
CopyFromUser:
    test rdx, rdx
    jz .Lcfu_done          /* zero-length: nothing to copy */
    mov rcx, rdx           /* rcx = count for rep */
    stac                     /* clear SMAP: allow user-page write */
    cld                      /* direction: forward */
.Lcfu_mem:
    rep movsb              /* movsb; DS = ES = kernel data segment */
    clac                     /* re-enable SMAP after the copy */
    xor eax, eax           /* 0: all bytes copied OK */
    ret
.Lcfu_done:
    xor eax, eax
    ret
.Lcfu_fixup:
    clac                     /* clean the FP state before returning */
    mov rax, rcx           /* return remaining count as "unfinished" */
    ret
.size CopyFromUser, .-CopyFromUser

.pushsection .extable, "a", @progbits
.balign 8
.quad .Lcfu_mem          /* faulting RIP -> fixup */
.quad .Lcfu_fixup
.popsection

/* uint64_t CopyToUser(void *Dst, const void *Src, uint64_t Len); */
.global CopyToUser
.type CopyToUser, @function
CopyToUser:
    test rdx, rdx
    jz .Lctu_done
    mov rcx, rdx
    stac
    cld
.Lctu_mem:
    rep movsb
    clac
    xor eax, eax
    ret
.Lctu_done:
    xor eax, eax
    ret
.Lctu_fixup:
    clac
    mov rax, rcx
    ret
.size CopyToUser, .-CopyToUser

.pushsection .extable, "a", @progbits
.balign 8
.quad .Lctu_mem
.quad .Lctu_fixup
.popsection

/* int64_t StrnlenUser(const char *Src, uint64_t Max); */
.global StrnlenUser
.type StrnlenUser, @function
StrnlenUser:
    test rsi, rsi
    jz .Lslu_done          /* Max == 0: no scan */
    mov rcx, rsi           /* rcx = max */
    xor eax, eax           /* al = 0 (byte value to scan for) */
    stac
    cld
.Lslu_mem:
    repne scasb            /* scan until byte == al or rcx == 0 */
    clac
    jz .Lslu_notfound      /* ZF clear: NUL not in the window */
    /* Found NUL: length counts bytes BEFORE the NUL */
    mov rax, rsi
    sub rax, rcx           /* rax = Max - remaining */
    dec rax                /* subtract the NUL itself */
    ret
.Lslu_notfound:
    /* NUL not found within Max bytes: length is Max */
    mov rax, rsi
    sub rax, rcx
    ret
.Lslu_done:
    xor eax, eax           /* 0 characters before NUL (empty string) */
    ret
.Lslu_fixup:
    clac
    mov rax, -1            /* -1 signals fault to the wrapper */
    ret
.size StrnlenUser, .-StrnlenUser

.pushsection .extable, "a", @progbits
.balign 8
.quad .Lslu_mem
.quad .Lslu_fixup
.popsection

/* Stubs for the wrapper functions in usercopy.c. The wrappers are
 * implemented in usercopy.c (they call the Raw primitives directly).
 * These stubs exist only to keep the "Raw" names defined for the linker. */
.global CopyFromUserWrapper
.type CopyFromUserWrapper, @function
CopyFromUserWrapper:
    jmp CopyFromUser
.size CopyFromUserWrapper, .-CopyFromUserWrapper

.global CopyToUserWrapper
.type CopyToUserWrapper, @function
CopyToUserWrapper:
    jmp CopyToUser
.size CopyToUserWrapper, .-CopyToUserWrapper

.global StrnlenUserWrapper
.type StrnlenUserWrapper, @function
StrnlenUserWrapper:
    jmp StrnlenUser
.size StrnlenUserWrapper, .-StrnlenUserWrapper
