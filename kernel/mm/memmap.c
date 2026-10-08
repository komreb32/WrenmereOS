/* SPDX-License-Identifier: BSD-3-Clause */
// Physical memory map built from the BootInfo E820 data: sorted by base,
// no overlaps (non-usable wins), same-type neighbours merged and usable
// regions aligned inward to 4 KiB.
#include <bootinfo.h>
#include <kernel/mm/memmap.h>
#include <memlayout.h>

extern uint8_t KernelPhysStart[];
extern uint8_t KernelPhysEnd[];

#define PageSize          4096ull
#define PageMask          (PageSize - 1ull)
#define E820EntrySize     24u
#define E820TypeOffset    16u
#define MemMapMaxRaw      32u         // stage2 caps the raw map at 32 entries
#define MemMapReserved    0x2u        // injected ranges use E820 "reserved"
#define MemMapLowEnd      0x00100000ull  // first MiB stays reserved
#define MemMapScratchBase 0x00004000ull  // BootInfo block
#define MemMapScratchEnd  0x00006000ull  // + E820 map/count (0x4000-0x5FFF)

typedef struct
{
    uint64_t Base;
    uint64_t End; // exclusive
    uint32_t Type;
} Range;

static MemMapRegion Regions[MemMapMaxRegions];
static uint32_t RegionCount;
static uint64_t UsableEnd;
static uint64_t UsablePagesTotal;
static uint64_t PhysicalEnd;


static Range Inputs[MemMapMaxRaw + 4]; // raw entries + injected ranges
static uint32_t InputCount;
static uint64_t Bounds[2 * (MemMapMaxRaw + 4)];

// Byte-wise readers: the raw 24-byte E820 entries are not 8-byte aligned.
static uint64_t Read64(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static uint32_t Read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Register one [Base, Base + Length) range; empty or wrapping ones are
// dropped, overflow of the input list is an error.
static int AddRange(uint64_t base, uint64_t length, uint32_t type)
{
    if (length == 0 || base > UINT64_MAX - length)
        return 0;
    if (InputCount >= MemMapMaxRaw + 4)
        return -1;
    Inputs[InputCount].Base = base;
    Inputs[InputCount].End = base + length;
    Inputs[InputCount].Type = type;
    InputCount++;
    return 0;
}

static void SortBounds(uint32_t count)
{
    uint32_t i;
    uint32_t j;

    for (i = 1; i < count; i++) {
        uint64_t key = Bounds[i];

        j = i;
        while (j > 0 && Bounds[j - 1] > key) {
            Bounds[j] = Bounds[j - 1];
            j--;
        }
        Bounds[j] = key;
    }
}

int MemMapBuild(void)
{
    const BootInfo *info = (const BootInfo *)PhysToVirt(BootInfoPhys);
    uint32_t raw_count;
    uint32_t bounds_count;
    uint32_t i;
    uint32_t j;

    RegionCount = 0;
    UsableEnd = 0;
    UsablePagesTotal = 0;
    PhysicalEnd = 0;
    InputCount = 0;

    if (info->Magic != BootInfoMagic || info->Size != sizeof(BootInfo))
        return -1;
    raw_count = info->MemMapCount;
    if (raw_count > MemMapMaxRaw)
        return -1;

    for (i = 0; i < raw_count; i++) {
        const uint8_t *entry = (const uint8_t *)PhysToVirt(
            info->MemMapPhys + (uint64_t)i * E820EntrySize);
        uint64_t base = Read64(entry);
        uint64_t len  = Read64(entry + 8);

        if (len > 0 && base + len > PhysicalEnd)
            PhysicalEnd = base + len;

        if (AddRange(base, len, Read32(entry + E820TypeOffset)) != 0)
            return -1;
    }

    // Reserved ranges the generated map must always contain.
    if (AddRange(0, MemMapLowEnd, MemMapReserved) != 0)
        return -1;
    if (AddRange(MemMapScratchBase, MemMapScratchEnd - MemMapScratchBase,
                 MemMapReserved) != 0)
        return -1;
    // Stage2's legacy KernelPhysEnd records a segment's start, not its end.
    // The linker provides the complete physical image, including our stack.
    if (AddRange((uint64_t)(uintptr_t)KernelPhysStart,
                 (uint64_t)(uintptr_t)KernelPhysEnd -
                 (uint64_t)(uintptr_t)KernelPhysStart, MemMapReserved) != 0)
        return -1;
    if (info->InitrdSize != 0 && info->InitrdPhys != 0 &&
        AddRange(info->InitrdPhys, info->InitrdSize, MemMapReserved) != 0)
        return -1;

    // Boundary sweep: every elementary piece takes the type of its
    // covering ranges (non-usable wins over usable); uncovered gaps stay
    // holes. Adjacent pieces of the same type merge as they are emitted.
    bounds_count = 0;
    for (i = 0; i < InputCount; i++) {
        Bounds[bounds_count++] = Inputs[i].Base;
        Bounds[bounds_count++] = Inputs[i].End;
    }
    SortBounds(bounds_count);
    j = 0;
    for (i = 0; i < bounds_count; i++)
        if (j == 0 || Bounds[i] != Bounds[j - 1])
            Bounds[j++] = Bounds[i];
    bounds_count = j;

    for (i = 0; i + 1 < bounds_count; i++) {
        uint64_t start = Bounds[i];
        uint64_t end = Bounds[i + 1];
        uint32_t type = 0; // 0 = hole
        uint32_t k;

        for (k = 0; k < InputCount; k++) {
            if (Inputs[k].Base > start || Inputs[k].End < end)
                continue; // does not cover the whole piece
            if (Inputs[k].Type != MemMapTypeUsable) {
                type = Inputs[k].Type;
                break; // non-usable wins
            }
            type = MemMapTypeUsable;
        }
        if (type == 0)
            continue;

        if (RegionCount != 0) {
            MemMapRegion *prev = &Regions[RegionCount - 1];

            if (prev->Type == type && prev->Base + prev->Length == start) {
                prev->Length += end - start;
                continue;
            }
        }
        if (RegionCount >= MemMapMaxRegions)
            return -1;
        Regions[RegionCount].Base = start;
        Regions[RegionCount].Length = end - start;
        Regions[RegionCount].Type = type;
        RegionCount++;
    }

    // Align usable regions inward to 4 KiB, drop the ones smaller than a
    // page and collect the usable statistics.
    j = 0;
    for (i = 0; i < RegionCount; i++) {
        uint64_t base = Regions[i].Base;
        uint64_t end = base + Regions[i].Length;
        uint32_t type = Regions[i].Type;

        if (type == MemMapTypeUsable) {
            base = (base + PageMask) & ~PageMask;
            end &= ~PageMask;
            if (end <= base)
                continue; // smaller than one page: drop
            if (end > UsableEnd)
                UsableEnd = end;
            UsablePagesTotal += (end - base) / PageSize;
        }
        Regions[j].Base = base;
        Regions[j].Length = end - base;
        Regions[j].Type = type;
        j++;
    }
    RegionCount = j;

    return 0;
}

const MemMapRegion *MemMapRegions(void)
{
    return Regions;
}

uint32_t MemMapCount(void)
{
    return RegionCount;
}

uint64_t MemMapUsableEnd(void)
{
    return UsableEnd;
}

uint64_t MemMapUsablePages(void)
{
    return UsablePagesTotal;
}

uint64_t MemMapPhysicalEnd(void)
{
    return PhysicalEnd;
}
