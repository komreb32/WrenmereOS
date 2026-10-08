/* SPDX-License-Identifier: BSD-3-Clause */
// 64-bit paging in Rust: access all page-table frames through the direct map.
#![no_std]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]

pub const PagingPageSize: u64 = 4096;
const PagingHardwarePageSize: u64 = 4096;
pub const PagingPresent: u64 = 0x001;
pub const PagingWritable: u64 = 0x002;
pub const PagingUser: u64 = 0x004;

const EntryCount: usize = 512;

static mut ActivePml4: u64 = 0;

unsafe extern "C" {
    fn PageAlloc() -> u64;
    fn PhysToVirt(Phys: u64) -> *mut core::ffi::c_void;
    fn VirtToPhys(Virt: *const core::ffi::c_void) -> u64;
}

unsafe fn ReadCr3() -> u64 {
    let Value: u64;
    unsafe { core::arch::asm!("mov {}, cr3", out(reg) Value, options(nostack, preserves_flags)) };
    Value
}

unsafe fn Invlpg(Virt: u64) {
    unsafe { core::arch::asm!("invlpg [{}]", in(reg) Virt, options(nostack, preserves_flags)) };
}

unsafe fn PtAlloc() -> *mut u64 {
    unsafe {
        let Frame = PageAlloc();
        if Frame == 0 {
            return core::ptr::null_mut();
        }
        let Table = PhysToVirt(Frame) as *mut u64;
        let mut i = 0;
        while i < EntryCount {
            core::ptr::write_volatile(Table.add(i), 0);
            i += 1;
        }
        Table
    }
}

unsafe fn EntryAddr(Entry: u64) -> u64 {
    Entry & 0x000FFFFFFFFFF000
}

unsafe fn ReadVolatile(Addr: *const u64) -> u64 {
    unsafe { core::ptr::read_volatile(Addr) }
}

unsafe fn WriteVolatile(Addr: *mut u64, Value: u64) {
    unsafe { core::ptr::write_volatile(Addr, Value) }
}

unsafe fn TableFor(Dir: *mut u64, Index: usize, Create: bool) -> *mut u64 {
    unsafe {
        let Entry = ReadVolatile(Dir.add(Index));
        if Entry & PagingPresent != 0 {
            if Entry & 0x80 == 0 {
                return PhysToVirt(EntryAddr(Entry)) as *mut u64;
            }
            if !Create {
                return core::ptr::null_mut();
            }
            // Split a 2MB entry so 4KB children can live under it.
            let Base = Entry & 0x000FFFFFFFE00000;
            let Pt = PtAlloc();
            if Pt.is_null() {
                return core::ptr::null_mut();
            }
            let Child = (Entry & (PagingPresent | PagingWritable | PagingUser)) | PagingPresent;
            let mut i = 0;
            while i < EntryCount {
                WriteVolatile(Pt.add(i), (Base + (i as u64) * PagingHardwarePageSize) | Child);
                i += 1;
            }
            WriteVolatile(Dir.add(Index), VirtToPhys(Pt.cast()) | PagingPresent | PagingWritable);
            return Pt;
        }
        if !Create {
            return core::ptr::null_mut();
        }
        let Pt = PtAlloc();
        if Pt.is_null() {
            return core::ptr::null_mut();
        }
        WriteVolatile(Dir.add(Index), VirtToPhys(Pt.cast()) | PagingPresent | PagingWritable);
        Pt
    }
}

unsafe fn Walk(Virt: u64, Create: bool) -> *mut u64 {
    unsafe {
        if ActivePml4 == 0 {
            return core::ptr::null_mut();
        }
        let Pdpt = TableFor(ActivePml4 as *mut u64, ((Virt >> 39) & 0x1FF) as usize, Create);
        if Pdpt.is_null() {
            return core::ptr::null_mut();
        }
        let Pd = TableFor(Pdpt, ((Virt >> 30) & 0x1FF) as usize, Create);
        if Pd.is_null() {
            return core::ptr::null_mut();
        }
        // Return the PT itself. Bits 12..20 select its leaf entry in the
        // caller, not another level of page tables.
        TableFor(Pd, ((Virt >> 21) & 0x1FF) as usize, Create)
    }
}

// One allocator frame maps directly to one hardware page-table entry.
#[no_mangle]
pub unsafe extern "C" fn PagingMap(Virt: u64, Phys: u64, Flags: u64) -> i32 {
    unsafe {
        if Virt & (PagingPageSize - 1) != 0 || Phys & (PagingPageSize - 1) != 0 {
            return -1;
        }
        let Pt = Walk(Virt, true);
        if Pt.is_null() {
            return -1;
        }
        let Index = ((Virt >> 12) & 0x1FF) as usize;
        if Index >= EntryCount {
            return -1;
        }
        let EntryFlags = (Flags & 0xFFF) | PagingPresent;
        WriteVolatile(Pt.add(Index), Phys | EntryFlags);
        Invlpg(Virt);
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn PagingUnmap(Virt: u64) -> i32 {
    unsafe {
        if Virt & (PagingPageSize - 1) != 0 {
            return -1;
        }
        let Pt = Walk(Virt, false);
        if Pt.is_null() {
            return -1;
        }
        let Index = ((Virt >> 12) & 0x1FF) as usize;
        if Index >= EntryCount {
            return -1;
        }
        WriteVolatile(Pt.add(Index), 0);
        Invlpg(Virt);
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn PagingTranslate(Virt: u64, PhysOut: *mut u64) -> i32 {
    unsafe {
        if PhysOut.is_null() {
            return -1;
        }
        let Pt = Walk(Virt & !(PagingPageSize - 1), false);
        if Pt.is_null() {
            return -1;
        }
        let Index = ((Virt >> 12) & 0x1FF) as usize;
        let Entry = ReadVolatile(Pt.add(Index));
        if Entry & PagingPresent == 0 {
            return -1;
        }
        *PhysOut = EntryAddr(Entry) | (Virt & (PagingPageSize - 1));
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn PagingInit() -> i32 {
    unsafe {
        ActivePml4 = PhysToVirt(ReadCr3() & 0x000FFFFFFFFFF000) as u64;
        if ActivePml4 == 0 { -1 } else { 0 }
    }
}

#[no_mangle]
pub unsafe extern "C" fn PagingIsEnabled() -> i32 {
    let Value: u64;
    unsafe { core::arch::asm!("mov {}, cr0", out(reg) Value, options(nostack, preserves_flags)) };
    if Value & 0x80000000 != 0 { 1 } else { 0 }
}
