/* SPDX-License-Identifier: BSD-3-Clause */
// Checked user-memory access over the Raw ASM primitives in usercopy.S.
//
// UserRangeOk() gates every wrapper so kernel addresses never reach the
// SMAP window; ExtableFind() lets the future #PF handler resume at the
// fixup recorded in .extable (kept address-sorted by kernel.ld).
#include <usercopy.h>
#include <errors.h>

uint64_t ExtableFind(uint64_t Rip)
{
    const ExtableEntry *Lo = ExtableStart;
    const ExtableEntry *Hi = ExtableEnd;

    if (Lo == 0 || Hi == 0 || Lo >= Hi)
        return 0;

    // Open interval [Lo, Hi): narrow until one candidate remains.
    while (Hi - Lo > 1)
    {
        const ExtableEntry *Mid = Lo + (uint64_t)(Hi - Lo) / 2;

        if (Mid->Addr <= Rip)
            Lo = Mid;
        else
            Hi = Mid;
    }

    return Lo->Addr == Rip ? Lo->Fixup : 0;
}

int UserRangeOk(uint64_t Addr, uint64_t Len)
{
    // Non-empty, no wraparound, and strictly below the user limit.
    // Addr + Len == UserAddrLimit is fine: the end is exclusive, and Len
    // bytes at exactly the limit would need End > Limit, which fails below.
    if (Len == 0)
        return 0;
    if (Addr >= UserAddrLimit)
        return 0;
    if (Len > UserAddrLimit - Addr)
        return 0;
    return 1;
}

int CopyFromUser(void *Dst, const void *Src, uint64_t Len)
{
    // Both ends checked: the user source range and the kernel destination.
    // Len == 0 with any pointers is a no-op success (matches the Raw stub).
    if (Len == 0)
        return ErrOk;
    if (Dst == 0 || Src == 0)
        return ErrFault;
    if (!UserRangeOk((uint64_t)(uintptr_t)Src, Len))
        return ErrFault;
    return CopyFromUser(Dst, Src, Len) == 0 ? ErrOk : ErrFault;
}

int CopyToUser(void *Dst, const void *Src, uint64_t Len)
{
    if (Len == 0)
        return ErrOk;
    if (Dst == 0 || Src == 0)
        return ErrFault;
    if (!UserRangeOk((uint64_t)(uintptr_t)Dst, Len))
        return ErrFault;
    return CopyToUser(Dst, Src, Len) == 0 ? ErrOk : ErrFault;
}

int StrnlenUser(const char *Src, uint64_t Max, uint64_t *LenOut)
{
    int64_t Got;

    // LenOut itself is a kernel out-pointer: fail fast before touching user.
    if (LenOut == 0)
        return ErrFault;
    if (Src == 0)
        return ErrFault;
    if (Max == 0)
    {
        *LenOut = 0;
        return ErrOk;
    }
    // Whole scan window must be user memory: scasb can legally walk it all.
    if (!UserRangeOk((uint64_t)(uintptr_t)Src, Max))
        return ErrFault;

    Got = StrnlenUserRaw(Src, Max);
    if (Got < 0)
        return ErrFault;
    *LenOut = (uint64_t)Got;
    return ErrOk;
}
