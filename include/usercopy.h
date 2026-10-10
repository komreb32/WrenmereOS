/* SPDX-License-Identifier: BSD-3-Clause */
// Checked user-memory access: range gate plus .extable fault recovery.
//
// Raw ASM primitives (kernel/mm/usercopy.S, SMAP bracketed, .extable
// covered) report the fault; the public wrappers here translate it to
// ErrFault from <errors.h>. The future #PF handler redirects RIP through
// ExtableFind(); nothing here installs that handler yet.
#ifndef USERCOPY_H
#define USERCOPY_H

#include <lib/stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Canonical user range, end exclusive (docs/abi.md).
#define UserAddrLimit 0x0000800000000000ULL

// One sorted .extable entry: faulting RIP -> fixup RIP.
typedef struct {
    uint64_t Addr;  // instruction touching user memory
    uint64_t Fixup; // resume here after the fault
} ExtableEntry;

extern const ExtableEntry ExtableStart[];
extern const ExtableEntry ExtableEnd[];

// Fixup for a faulting RIP, or 0 when no entry covers it. Binary search
// over the link-sorted .extable; NULL-safe.
uint64_t ExtableFind(uint64_t Rip);

// True when [Addr, Addr + Len) is a non-empty user range with no overflow.
int UserRangeOk(uint64_t Addr, uint64_t Len);

// Raw ASM primitives (usercopy.S). Copies return bytes NOT copied
// (0 = all done, >0 = fault cut it short); Strnlen returns 0..Max or -1.
uint64_t CopyFromUserRaw(void *Dst, const void *Src, uint64_t Len);
uint64_t CopyToUserRaw(void *Dst, const void *Src, uint64_t Len);
int64_t StrnlenUserRaw(const char *Src, uint64_t Max);

// Checked wrappers. 0 on success, ErrFault when the range is not user
// memory or the copy faulted partway.
int CopyFromUser(void *Dst, const void *Src, uint64_t Len);
int CopyToUser(void *Dst, const void *Src, uint64_t Len);
// Checked bounded strlen: *LenOut gets 0..Max, result 0 ok / ErrFault when
// the user range faults before any NUL (StrnlenUserRaw reports -1).
int StrnlenUser(const char *Src, uint64_t Max, uint64_t *LenOut);

#ifdef __cplusplus
}
#endif

#endif
