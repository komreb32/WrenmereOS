/* SPDX-License-Identifier: BSD-3-Clause */
// Tiny glue so C can bring the Rust memory manager up in one call.
#include <kernel/mm/memmap.h>
#include <kernel/mm/mm.h>
#include <kernel/mm/page_alloc.h>
#include <kernel/mm/paging.h>
#include <lib/stdint.h>
#include <memlayout.h>

// Absolute physical linker symbols, including the .bss bootstrap stack.
extern uint8_t KernelPhysStart[];
extern uint8_t KernelPhysEnd[];

#define MmMapBase    0xFFFFC00000000000ULL // outside the direct map, initially unmapped

static uint64_t TotalUsablePages;

// Export one implementation for both the C kernel and the Rust MM objects.
void *PhysToVirt(uint64_t Phys)
{
    return (void *)(uintptr_t)(DirectMapBase + Phys);
}

uint64_t VirtToPhys(const void *Virt)
{
    return (uint64_t)(uintptr_t)Virt - DirectMapBase;
}

uint64_t MmTotalPages(void)
{
    return TotalUsablePages;
}

uint64_t MmTotalRamBytes(void)
{
    return MemMapPhysicalEnd();
}

uint64_t MmUsableRam(void)
{
    return (uint64_t)PageAllocFreeCount() * PageSize;
}

uint64_t MmUsedRam(void)
{
    uint64_t total = MmTotalRamBytes();
    uint64_t free  = MmUsableRam();
    return total > free ? total - free : 0;
}


// The #PF handler fires here if the new map does not work: recover by
// dropping the map so MmInit can still report failure gracefully.
void MmProbeFault(void)
{
    PagingUnmap(MmMapBase);
}

// Translate must agree with the entries before mapping anything.
static int MmCheckTranslate(uint64_t Virt, uint64_t ExpectPhys)
{
    uint64_t Got = 0xAAAAAAAAAAAAAAAAULL;

    if (PagingTranslate(Virt, &Got) != 0)
        return -1;

    return Got == ExpectPhys ? 0 : -1;
}

int MmInit(void)
{
    const MemMapRegion *Map;
    uint64_t MaxEnd;
    uint64_t TotalFrames;
    uint64_t BitmapBytes;
    uint64_t BitmapPages;
    uint64_t Arena;
    uint64_t Frame;
    uint32_t Count;
    uint32_t i;
    int R;

    // Build the cleaned map from the BootInfo E820 data first.
    if (MemMapBuild() != 0)
        return -1;

    if (PagingInit() != 0)
        return -1;

    Count = MemMapCount();
    MaxEnd = MemMapUsableEnd();

    // Pool covers all physical memory up to the highest usable frame.
    // Bitmap tracks every 4 KiB frame up to MaxEnd, so size depends on total RAM.
    if (MaxEnd == 0)
        return -1;
    TotalFrames = MaxEnd / PageSize;
    BitmapBytes = ((TotalFrames + 31ULL) / 32ULL) * sizeof(uint32_t);
    BitmapPages = (BitmapBytes + PageSize - 1ULL) / PageSize;

    // Place the bitmap in the first large-enough usable region after KernelPhysEnd.
    // MemMapBuild already carved out the kernel, stage2, and boot scratch areas.
    Arena = 0;
    Map = MemMapRegions();
    for (i = 0; i < Count; i++)
    {
        uint64_t RegionBase;
        uint64_t RegionEnd;
        uint64_t AlignedBase;
        uint64_t AlignedEnd;

        if (Map[i].Type != MemMapTypeUsable)
            continue;

        RegionBase = Map[i].Base;
        RegionEnd = RegionBase + Map[i].Length;

        // Align inward to whole pages so we don't accidentally place the
        // bitmap on memory that isn't fully owned by this region.
        AlignedBase = (RegionBase + PageSize - 1ULL) & ~(PageSize - 1ULL);
        AlignedEnd = RegionEnd & ~(PageSize - 1ULL);

        if (AlignedEnd <= AlignedBase)
            continue;

        // Skip any region that ends before KernelPhysEnd.
        if (AlignedEnd <= (uint64_t)(uintptr_t)KernelPhysEnd)
            continue;

        // If the region starts before KernelPhysEnd, clip to the part after it.
        if (AlignedBase < (uint64_t)(uintptr_t)KernelPhysEnd)
            AlignedBase = (uint64_t)(uintptr_t)KernelPhysEnd;

        AlignedBase = (AlignedBase + PageSize - 1ULL) & ~(PageSize - 1ULL);

        if (AlignedBase < AlignedEnd && (AlignedEnd - AlignedBase) >= BitmapPages * PageSize)
        {
            Arena = AlignedBase;
            break;
        }
    }

    if (Arena == 0)
        return -1;

    if (PageAllocInit(MaxEnd, Arena) != 0)
        return -1;

    // Add all usable regions to the allocator, then count total usable pages.
    TotalUsablePages = 0;
    Map = MemMapRegions();
    for (i = 0; i < Count; i++)
    {
        uint64_t RegionBase;
        uint64_t RegionEnd;

        if (Map[i].Type != MemMapTypeUsable)
            continue;

        RegionBase = Map[i].Base;
        RegionEnd = RegionBase + Map[i].Length;

        // All usable regions are part of the pool now; MemMapBuild already
        // carved out reserved areas (first MiB, kernel, stage2, tables, etc).
        TotalUsablePages += (RegionEnd - RegionBase) / PageSize;
        PageAddRegion(RegionBase, RegionEnd - RegionBase);
    }

    // Reserve the areas that must stay out of the pool:
    // - The bitmap itself (we placed it in usable memory)
    // - The first MiB (stage2, BootInfo, E820, early page tables, VGA)
    // - The kernel image and stack (KernelPhysStart to KernelPhysEnd)
    PageReserveRange(Arena, BitmapPages * PageSize);
    PageReserveRange(0x0, 0x100000);
    PageReserveRange((uint64_t)(uintptr_t)KernelPhysStart,
                     (uint64_t)(uintptr_t)KernelPhysEnd -
                     (uint64_t)(uintptr_t)KernelPhysStart);

    if (PageAllocFreeCount() == 0)
        return -1;

    // Map one 4 KiB page, then prove the walker and the bytes agree.
    Frame = PageAlloc();
    if (Frame == 0)
        return -1;

    if (PagingMap(MmMapBase, Frame, PagingWritable) != 0)
    {
        PageFree(Frame);
        return -1;
    }

    if (MmCheckTranslate(MmMapBase, Frame) != 0)
    {
        PagingUnmap(MmMapBase);
        PageFree(Frame);
        return -1;
    }

    *(volatile uint64_t *)MmMapBase = 0x0123456789ABCDEFULL;
    R = *(volatile uint64_t *)MmMapBase == 0x0123456789ABCDEFULL &&
        *(volatile uint64_t *)PhysToVirt(Frame) == 0x0123456789ABCDEFULL ? 0 : -1;

    PagingUnmap(MmMapBase);
    PageFree(Frame);

    return R;
}
