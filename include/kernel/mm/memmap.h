/* SPDX-License-Identifier: BSD-3-Clause */
// Clean physical memory map built from the BootInfo E820 data.
#ifndef KERNEL_MM_MEMMAP_H
#define KERNEL_MM_MEMMAP_H

#include <lib/stdint.h>

// E820 type 1 means usable; every other value is treated as reserved.
#define MemMapTypeUsable 1u

// Output list cap. The raw map is capped at 32 entries and 4 reserved
// ranges are injected, so the boundary sweep produces at most 71 pieces.
#define MemMapMaxRegions 128u

typedef struct
{
    uint64_t Base;
    uint64_t Length;
    uint32_t Type; // MemMapTypeUsable or an E820 reserved type
} MemMapRegion;

// Reads the raw E820 entries from BootInfo and generates a list sorted by
// base with no overlaps: where regions collide the non-usable type wins,
// adjacent regions of the same type are merged and usable regions are
// aligned inward to 4 KiB. The first MiB, the kernel image, the boot
// scratch (0x4000-0x5FFF) and the initrd are injected as reserved.
// Returns 0 on success, -1 when BootInfo is invalid or the list overflows.
int MemMapBuild(void);

const MemMapRegion *MemMapRegions(void);
uint32_t MemMapCount(void);
uint64_t MemMapUsableEnd(void);    // highest usable end, exclusive
uint64_t MemMapUsablePages(void);  // total usable 4 KiB frames
uint64_t MemMapPhysicalEnd(void);  // highest end of ANY E820 region (total installed RAM)

#endif
