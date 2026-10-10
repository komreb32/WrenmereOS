/* SPDX-License-Identifier: BSD-3-Clause */
// Address spaces: one VMA list per space. Frames come from PageAlloc and
// mappings from PagingMap; the walker only sees the active CR3, so ranges
// are recorded per space but mapped into the live tables.
#![no_std]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]

pub const AsPageSize: u64 = 4096;
const AsPageMask: u64 = AsPageSize - 1;

// docs/abi.md: user space ends here, the mmap region starts here.
pub const AsUserLimit: u64 = 0x0000800000000000;
pub const AsMmapBase: u64 = 0x0000100000000000;
// Canonical higher half starts here; the gap between AsUserLimit and this
// base is never addressable, even for module images.
const AsKernelBase: u64 = 0xFFFF800000000000;

// VMA flags are page-table permission bits and pass straight to PagingMap.
pub const AsFlagPresent: u64 = 0x001;
pub const AsFlagWritable: u64 = 0x002;
pub const AsFlagUser: u64 = 0x004;
pub const AsFlagExecute: u64 = 0x200; // OS PTE bit; hardware NX is separate

// What a range is for: faults only refill Stack/Anonymous, shared ranges
// never give frames back, and only module images live in the higher half.
pub const VmaKindAnonymous: u32 = 0;
pub const VmaKindStack: u32 = 1;
pub const VmaKindShared: u32 = 2;
pub const VmaKindModule: u32 = 3;

pub const AsMaxVmas: usize = 64;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Vma {
    pub Start: u64, // inclusive
    pub End: u64,   // exclusive
    pub Flags: u64,
    pub Kind: u32,
}

impl Vma {
    pub const fn New() -> Vma {
        Vma { Start: 0, End: 0, Flags: 0, Kind: 0 }
    }
}

// Sorted by Start with no overlaps; an all-zero space is an empty space.
#[repr(C)]
pub struct AddrSpace {
    pub Vmas: [Vma; AsMaxVmas],
    pub VmaCount: u32,
}

impl AddrSpace {
    pub const fn New() -> AddrSpace {
        AddrSpace { Vmas: [Vma::New(); AsMaxVmas], VmaCount: 0 }
    }
}

unsafe extern "C" {
    fn PageAlloc() -> u64;
    fn PageFree(Address: u64);
    fn PagingMap(Virt: u64, Phys: u64, Flags: u64) -> i32;
    fn PagingUnmap(Virt: u64) -> i32;
    fn PagingTranslate(Virt: u64, PhysOut: *mut u64) -> i32;
    fn PhysToVirt(Phys: u64) -> *mut core::ffi::c_void;
}

// Round up to whole pages; callers reject the overflow case first.
const fn RoundUp(Size: u64) -> u64 {
    Size.wrapping_add(AsPageMask) & !AsPageMask
}

// Raw slot pointer: the kernel link has no Rust panic runtime, so every
// VMA access goes through pointers instead of checked indexing.
unsafe fn VmaAt(As: *mut AddrSpace, Index: usize) -> *mut Vma {
    unsafe { (*As).Vmas.as_mut_ptr().add(Index) }
}

unsafe fn Overlaps(As: *mut AddrSpace, Start: u64, End: u64) -> bool {
    unsafe {
        let Count = (*As).VmaCount as usize;
        let mut i = 0;
        while i < Count {
            let V = *VmaAt(As, i);
            if V.Start < End && Start < V.End {
                return true;
            }
            i += 1;
        }
        false
    }
}

// First fit: the lowest page-aligned address at or above From whose whole
// [Candidate, Candidate + Size) stays clear of every VMA and below Limit.
// 0 means no room.
unsafe fn FindHole(As: *mut AddrSpace, From: u64, Size: u64, Limit: u64) -> u64 {
    unsafe {
        if Size == 0 || Size > Limit {
            return 0;
        }
        // [Limit - Size, Limit) is the last Size-long run that clears Limit.
        let LastStart = Limit - Size;
        let mut Candidate = RoundUp(From);
        if Candidate == 0 {
            return 0; // From sat in the final page and rounding wrapped
        }
        loop {
            if Candidate > LastStart {
                return 0;
            }
            // Ends grow with Start (no overlaps), so the first VMA ending
            // past Candidate is the only one that can block the hole.
            let Count = (*As).VmaCount as usize;
            let mut i = 0;
            let mut Hit = Count;
            while i < Count {
                if (*VmaAt(As, i)).End > Candidate {
                    Hit = i;
                    break;
                }
                i += 1;
            }
            if Hit == Count {
                return Candidate;
            }
            let V = *VmaAt(As, Hit);
            if V.Start >= Candidate + Size {
                return Candidate;
            }
            Candidate = RoundUp(V.End);
            if Candidate == 0 {
                return 0;
            }
        }
    }
}

// Fresh frames carry stale data; anonymous memory must read as zeros.
unsafe fn ZeroFrame(Frame: u64) {
    unsafe {
        let Bytes = PhysToVirt(Frame) as *mut u8;
        let mut i: u64 = 0;
        while i < AsPageSize {
            core::ptr::write_volatile(Bytes.add(i as usize), 0);
            i += 1;
        }
    }
}

// Unmap [Start, End) page by page and hand the frames back unless the
// covering VMA is Shared: those frames back a shared object, not us.
// ForceFree is for rollbacks, where the frames are ours regardless.
unsafe fn UnmapRange(As: *mut AddrSpace, Start: u64, End: u64, ForceFree: bool) {
    unsafe {
        let mut Virt = Start;
        while Virt < End {
            let mut Phys = 0u64;
            if PagingTranslate(Virt, &mut Phys) == 0 {
                let V = AsFindVma(As, Virt);
                let Shared = !V.is_null() && (*V).Kind == VmaKindShared;
                PagingUnmap(Virt);
                if ForceFree || !Shared {
                    PageFree(Phys & !AsPageMask);
                }
            }
            Virt = match Virt.checked_add(AsPageSize) {
                Some(Next) => Next,
                None => break,
            };
        }
    }
}

// A [Start, End) pair is canonical when it sits wholly in the low half or
// wholly in the canonical higher half; anything touching the gap between
// AsUserLimit and AsKernelBase would fault on use.
const fn CanonicalRange(Start: u64, End: u64) -> bool {
    End <= AsUserLimit || Start >= AsKernelBase
}

// Pick [Start, End) for the range APIs. Fixed means exactly Hint, failing
// on any overlap; otherwise first fit from Hint, or from the mmap base in
// docs/abi.md when Hint is zero. Only module images may reach the higher
// half, and a non-fixed module allocation stays below it unless the caller
// already aimed up there. Returns 0 when no range qualifies.
unsafe fn ChooseRange(As: *mut AddrSpace, Hint: u64, Size: u64, Kind: u32, Fixed: bool) -> u64 {
    unsafe {
        if As.is_null() || Size == 0 {
            return 0;
        }
        let Size = RoundUp(Size);
        if Size == 0 {
            return 0; // rounding the size wrapped past the top
        }
        let High = Kind == VmaKindModule && (Fixed || Hint >= AsKernelBase);
        if Fixed {
            if Hint == 0 || Hint & AsPageMask != 0 {
                return 0;
            }
            if Size > u64::MAX - Hint {
                return 0;
            }
            let End = Hint + Size;
            if High {
                if !CanonicalRange(Hint, End) {
                    return 0;
                }
            } else if End > AsUserLimit {
                return 0;
            }
            if Overlaps(As, Hint, End) {
                return 0;
            }
            Hint
        } else if High {
            // Non-fixed module with an explicit high hint: behave like the
            // low-half search, but consult the canonical higher half first.
            let Start = FindHole(As, Hint.max(AsKernelBase), Size, u64::MAX);
            if Start != 0 {
                Start
            } else {
                FindHole(As, AsMmapBase, Size, AsUserLimit)
            }
        } else {
            // Spec: first fit from Hint, or from the mmap base when Hint is 0.
            let From = if Hint == 0 { AsMmapBase } else { Hint };
            FindHole(As, From, Size, AsUserLimit)
        }
    }
}

// Register [Start, End), rejecting overlaps and any range that reaches
// into the higher half; only module images may live up there, and even
// they must stay canonical.
#[no_mangle]
pub unsafe extern "C" fn AsAddVma(
    As: *mut AddrSpace,
    Start: u64,
    End: u64,
    Flags: u64,
    Kind: u32,
) -> i32 {
    unsafe {
        if As.is_null() || Start >= End {
            return -1;
        }
        if Kind != VmaKindModule {
            if End > AsUserLimit {
                return -1;
            }
        } else if !CanonicalRange(Start, End) {
            return -1;
        }
        let Count = (*As).VmaCount as usize;
        if Count >= AsMaxVmas || Overlaps(As, Start, End) {
            return -1;
        }
        // Insert in Start order; a free slot always exists by the check above.
        let mut At = Count;
        let mut i = 0;
        while i < Count {
            if (*VmaAt(As, i)).Start >= End {
                At = i;
                break;
            }
            i += 1;
        }
        let mut j = Count;
        while j > At {
            *VmaAt(As, j) = *VmaAt(As, j - 1);
            j -= 1;
        }
        *VmaAt(As, At) = Vma { Start: Start, End: End, Flags: Flags, Kind: Kind };
        (*As).VmaCount = (Count + 1) as u32;
        0
    }
}

// The VMA covering Addr, or null when nothing owns that address.
#[no_mangle]
pub unsafe extern "C" fn AsFindVma(As: *mut AddrSpace, Addr: u64) -> *mut Vma {
    unsafe {
        if As.is_null() {
            return core::ptr::null_mut();
        }
        let Count = (*As).VmaCount as usize;
        let mut i = 0;
        while i < Count {
            let Slot = VmaAt(As, i);
            if (*Slot).Start <= Addr && Addr < (*Slot).End {
                return Slot;
            }
            i += 1;
        }
        core::ptr::null_mut()
    }
}

// Remove [Start, End) from the list: a VMA fully inside is dropped, one
// sticking out is trimmed, and a VMA with the removal in its middle is
// split into two. Nothing to remove is still success.
#[no_mangle]
pub unsafe extern "C" fn AsRemoveVma(As: *mut AddrSpace, Start: u64, End: u64) -> i32 {
    unsafe {
        if As.is_null() || Start >= End {
            return -1;
        }
        let mut Count = (*As).VmaCount as usize;
        let mut i = 0;
        while i < Count {
            let V = *VmaAt(As, i);
            if V.End <= Start {
                i += 1;
                continue;
            }
            if V.Start >= End {
                break;
            }
            // Three shapes: covered (drop), a remainder on one side (trim),
            // or a hole punched in the middle (split into two).
            if V.Start >= Start && V.End <= End {
                let mut j = i;
                while j + 1 < Count {
                    *VmaAt(As, j) = *VmaAt(As, j + 1);
                    j += 1;
                }
                Count -= 1;
                continue; // re-examine the slot that slid into place
            }
            if V.Start < Start && V.End > End {
                if Count >= AsMaxVmas {
                    return -1; // no spare slot for the second half
                }
                let mut j = Count;
                while j > i + 1 {
                    *VmaAt(As, j) = *VmaAt(As, j - 1);
                    j -= 1;
                }
                Count += 1;
                (*VmaAt(As, i)).End = Start;
                *VmaAt(As, i + 1) = Vma {
                    Start: End,
                    End: V.End,
                    Flags: V.Flags,
                    Kind: V.Kind,
                };
                i += 2;
                continue;
            }
            // Trimmed: keep whichever remainder survives.
            if V.Start < Start {
                (*VmaAt(As, i)).End = Start;
            } else {
                (*VmaAt(As, i)).Start = End;
            }
            i += 1;
        }
        (*As).VmaCount = Count as u32;
        0
    }
}

// Allocate [Start, Start + Span) with fresh zeroed frames and record the
// VMA. Fixed means exactly Hint, failing on any overlap; otherwise first
// fit from Hint, or from the mmap base in docs/abi.md when Hint is zero.
// Returns the base address, or 0 when no range or frame could be had.
#[no_mangle]
pub unsafe extern "C" fn AsAllocRange(
    As: *mut AddrSpace,
    Hint: u64,
    Size: u64,
    Flags: u64,
    Kind: u32,
    Fixed: bool,
) -> u64 {
    unsafe {
        let Start = ChooseRange(As, Hint, Size, Kind, Fixed);
        if Start == 0 {
            return 0;
        }
        // ChooseRange already validated both terms and capped the end.
        let End = Start + RoundUp(Size);
        if AsAddVma(As, Start, End, Flags, Kind) != 0 {
            return 0;
        }
        let mut Virt = Start;
        while Virt < End {
            let Frame = PageAlloc();
            if Frame == 0 {
                UnmapRange(As, Start, Virt, true);
                AsRemoveVma(As, Start, End);
                return 0;
            }
            ZeroFrame(Frame);
            if PagingMap(Virt, Frame, Flags) != 0 {
                PageFree(Frame);
                UnmapRange(As, Start, Virt, true);
                AsRemoveVma(As, Start, End);
                return 0;
            }
            Virt += AsPageSize;
        }
        Start
    }
}

// Same range choice as AsAllocRange, but only records the VMA: no frames
// are requested and nothing is mapped. This backs shared memory, whose
// frames belong to the shared object and live outside this allocator.
#[no_mangle]
pub unsafe extern "C" fn AsReserveRange(
    As: *mut AddrSpace,
    Hint: u64,
    Size: u64,
    Flags: u64,
    Kind: u32,
    Fixed: bool,
) -> u64 {
    unsafe {
        let Start = ChooseRange(As, Hint, Size, Kind, Fixed);
        if Start == 0 {
            return 0;
        }
        let End = Start + RoundUp(Size);
        if AsAddVma(As, Start, End, Flags, Kind) != 0 {
            return 0;
        }
        Start
    }
}

// Unmap [Addr, Addr + Size), return the frames and carve the range out of
// the VMA list. Both bounds round outward to whole pages (frames are pages)
// and a Shared VMA only unmaps: its frames are never freed here.
#[no_mangle]
pub unsafe extern "C" fn AsFreeRange(As: *mut AddrSpace, Addr: u64, Size: u64) -> i32 {
    unsafe {
        if As.is_null() || Size == 0 || Size > u64::MAX - Addr {
            return -1;
        }
        let Start = Addr & !AsPageMask;
        let End = RoundUp(Addr + Size);
        if End <= Start {
            return -1; // rounding wrapped past the top of the address space
        }
        UnmapRange(As, Start, End, false);
        AsRemoveVma(As, Start, End)
    }
}

// Page-fault hook: back a demand-paged Stack/Anonymous page with one zeroed
// frame. The fault only counts as ours when the VMA grants the permission
// the access asked for and the page is not mapped yet; anything else (no
// VMA, wrong kind, missing write/user bit, already present) is the
// caller's problem and reports -1.
#[no_mangle]
pub unsafe extern "C" fn AsHandleFault(
    As: *mut AddrSpace,
    Addr: u64,
    Write: bool,
    User: bool,
) -> i32 {
    unsafe {
        let V = AsFindVma(As, Addr);
        if V.is_null() {
            return -1;
        }
        let Kind = (*V).Kind;
        if Kind != VmaKindStack && Kind != VmaKindAnonymous {
            return -1;
        }
        let Need = (if Write { AsFlagWritable } else { 0 })
            | (if User { AsFlagUser } else { 0 });
        if (*V).Flags & Need != Need {
            return -1;
        }
        let Page = Addr & !AsPageMask;
        let mut Phys = 0u64;
        if PagingTranslate(Page, &mut Phys) == 0 {
            return -1; // already present: not a missing page
        }
        let Frame = PageAlloc();
        if Frame == 0 {
            return -1;
        }
        ZeroFrame(Frame);
        if PagingMap(Page, Frame, (*V).Flags) != 0 {
            PageFree(Frame);
            return -1;
        }
        0
    }
}



