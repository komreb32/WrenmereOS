/* SPDX-License-Identifier: BSD-3-Clause */
// Rust frame allocator: 4 KiB frames, bitmap parked inside the arena itself.
#![no_std]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]

pub const PageSize: u64 = 4096;
const PageMask: u64 = PageSize - 1;

unsafe extern "C" {
    fn PhysToVirt(Phys: u64) -> *mut core::ffi::c_void;
}

// Physical bitmap address; convert at every CPU access, not in frame APIs.
static mut BitmapAddr: u64 = 0;
static mut BitmapWords: u32 = 0;
static mut ArenaBase: u64 = 0;
static mut ArenaPages: u32 = 0;
static mut FreeFrames: u32 = 0;
static mut AllocatorInitialized: bool = false;

unsafe fn Word(Index: u32) -> u32 {
    unsafe {
        let Ptr = PhysToVirt(BitmapAddr + (Index as u64) * 4) as *const u32;
        core::ptr::read_volatile(Ptr)
    }
}

unsafe fn WriteWord(Index: u32, Value: u32) {
    unsafe {
        let Ptr = PhysToVirt(BitmapAddr + (Index as u64) * 4) as *mut u32;
        core::ptr::write_volatile(Ptr, Value);
    }
}

unsafe fn SetBit(Frame: u32) {
    unsafe {
        let Value = Word(Frame / 32);
        if Value & (1 << (Frame % 32)) == 0 {
            WriteWord(Frame / 32, Value | (1 << (Frame % 32)));
            FreeFrames += 1;
        }
    }
}

unsafe fn ClearBit(Frame: u32) {
    unsafe {
        let Value = Word(Frame / 32);
        if Value & (1 << (Frame % 32)) != 0 {
            WriteWord(Frame / 32, Value & !(1 << (Frame % 32)));
            FreeFrames -= 1;
        }
    }
}

unsafe fn IsFree(Frame: u32) -> bool {
    unsafe { Word(Frame / 32) & (1 << (Frame % 32)) != 0 }
}

// Marks [0, MaxEnd) managed; everything starts reserved, regions are
// added with PageAddRegion so only real E820 RAM becomes usable.
#[no_mangle]
pub unsafe extern "C" fn PageAllocInit(MaxEnd: u64, Arena: u64) -> i32 {
    unsafe {
        AllocatorInitialized = false;
        ArenaPages = 0;
        FreeFrames = 0;
        if MaxEnd == 0 || Arena == 0 || Arena & PageMask != 0 {
            return -1;
        }
        let MaxFrames = (MaxEnd & !PageMask) / PageSize;
        if MaxFrames == 0 || MaxFrames > 0xFFFFFFFF {
            return -1;
        }
        ArenaBase = 0;
        ArenaPages = MaxFrames as u32;
        BitmapWords = (ArenaPages + 31) / 32;
        BitmapAddr = Arena;
        let BitmapBytes = (BitmapWords as u64) * 4;
        // No memset in freestanding: clear the bitmap by hand.
        let Bitmap = PhysToVirt(Arena) as *mut u8;
        let mut Offset = 0;
        while Offset < BitmapBytes {
            core::ptr::write_volatile(Bitmap.add(Offset as usize), 0);
            Offset += 1;
        }
        FreeFrames = 0;
        AllocatorInitialized = true;
        0
    }
}

// Frees every 4 KiB frame in [Base, Base + Length), rounding inward so only
// fully usable frames enter the pool. Returns frames added.
#[no_mangle]
pub unsafe extern "C" fn PageAddRegion(Base: u64, Length: u64) -> u32 {
    unsafe {
        if ArenaPages == 0 || Length == 0 {
            return 0;
        }
        let End = Base.wrapping_add(Length);
        if End < Base {
            return 0;
        }
        let First = (Base + PageMask) & !PageMask;
        let Last = End & !PageMask;
        if Last <= First {
            return 0;
        }
        let Limit = (ArenaPages as u64) * PageSize;
        let Last = Last.min(Limit);
        if Last <= First {
            return 0;
        }
        let mut Added = 0;
        let mut Addr = First;
        while Addr < Last {
            let Frame = (Addr / PageSize) as u32;
            if Frame < ArenaPages {
                let Before = FreeFrames;
                SetBit(Frame);
                if FreeFrames != Before {
                    Added += 1;
                }
            }
            Addr += PageSize;
        }
        Added
    }
}

// Reserves [Base, Base + Length), rounding outward so partial frames die too.
unsafe fn Reserve(Base: u64, Length: u64) {
    unsafe {
        if ArenaPages == 0 || Length == 0 {
            return;
        }
        let End = Base.wrapping_add(Length);
        if End < Base {
            return;
        }
        let First = Base & !PageMask;
        let Last = (End + PageMask) & !PageMask;
        let mut Addr = First;
        while Addr < Last {
            if Addr >= ArenaBase {
                let Frame = ((Addr - ArenaBase) / PageSize) as u32;
                if Frame < ArenaPages {
                    ClearBit(Frame);
                }
            }
            Addr += PageSize;
        }
    }
}

// First fit run of contiguous 4 KiB frames, 0 on failure.
#[no_mangle]
pub unsafe extern "C" fn PageAllocPages(Count: u32) -> u64 {
    unsafe {
        if Count == 0 || ArenaPages == 0 || Count > FreeFrames || Count > ArenaPages {
            return 0;
        }
        let mut Start: u32 = 0;
        while Start <= ArenaPages - Count {
            let mut Run: u32 = 0;
            while Run < Count {
                if !IsFree(Start + Run) {
                    break;
                }
                Run += 1;
            }
            if Run == Count {
                let mut i = 0;
                while i < Count {
                    ClearBit(Start + i);
                    i += 1;
                }
                return ArenaBase + (Start as u64) * PageSize;
            }
            Start += Run + 1;
        }
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn PageAlloc() -> u64 {
    unsafe { PageAllocPages(1) }
}

// Hint version from the example heap: exact hit or 0, no first-fit scan.
#[no_mangle]
pub unsafe extern "C" fn PageAllocPagesAt(Hint: u64, Count: u32) -> u64 {
    unsafe {
        if Count == 0 || !AllocatorInitialized || Hint == 0 || Hint & PageMask != 0 {
            return 0;
        }
        if Hint < ArenaBase {
            return 0;
        }
        let Start = ((Hint - ArenaBase) / PageSize) as u32;
        if Start >= ArenaPages || (Count as u64) > (ArenaPages - Start) as u64 {
            return 0;
        }
        let mut i = 0;
        while i < Count {
            if !IsFree(Start + i) {
                return 0;
            }
            i += 1;
        }
        let mut j = 0;
        while j < Count {
            ClearBit(Start + j);
            j += 1;
        }
        Hint
    }
}

unsafe fn FreeRange(Address: u64, Count: u32) {
    unsafe {
        if Address == 0 || Address & PageMask != 0 || Count == 0 || !AllocatorInitialized {
            return;
        }
        if Address < ArenaBase {
            return;
        }
        let Start = ((Address - ArenaBase) / PageSize) as u32;
        if Start >= ArenaPages || (Count as u64) > (ArenaPages - Start) as u64 {
            return;
        }
        let mut i = 0;
        while i < Count {
            SetBit(Start + i);
            i += 1;
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn PageFree(Address: u64) {
    unsafe { FreeRange(Address, 1) }
}

#[no_mangle]
pub unsafe extern "C" fn PageFreePages(Address: u64, Count: u32) {
    unsafe { FreeRange(Address, Count) }
}

#[no_mangle]
pub unsafe extern "C" fn PageAllocFreeCount() -> u32 {
    unsafe { FreeFrames }
}

#[no_mangle]
pub unsafe extern "C" fn PageArenaBase() -> u64 {
    unsafe { ArenaBase }
}

#[no_mangle]
pub unsafe extern "C" fn PageArenaPages() -> u32 {
    unsafe { ArenaPages }
}

#[no_mangle]
pub unsafe extern "C" fn PageReserveRange(Base: u64, Length: u64) {
    unsafe { Reserve(Base, Length) }
}

#[panic_handler]
fn OnPanic(_: &core::panic::PanicInfo) -> ! {
    loop {
        unsafe { core::arch::asm!("hlt") };
    }
}
