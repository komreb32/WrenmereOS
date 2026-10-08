/* SPDX-License-Identifier: BSD-3-Clause */
// Physical frame allocator: 4 KiB frames tracked by a bitmap inside the arena.
#ifndef KERNEL_MM_PAGE_ALLOC_H
#define KERNEL_MM_PAGE_ALLOC_H

#include <lib/stdint.h>

#define PageSize 4096u

// Arena is the physical bitmap home, accessed through PhysToVirt. All frame
// addresses in this API are physical; the pool is every usable E820 region.
int PageAllocInit(uint64_t MaxEnd, uint64_t Arena);
uint32_t PageAddRegion(uint64_t Base, uint64_t Length);
uint64_t PageAlloc(void);
uint64_t PageAllocPages(uint32_t Count);
uint64_t PageAllocPagesAt(uint64_t Hint, uint32_t Count);
void PageFree(uint64_t Address);
void PageFreePages(uint64_t Address, uint32_t Count);
void PageReserveRange(uint64_t Base, uint64_t Length);
uint64_t PageArenaBase(void);
uint32_t PageArenaPages(void);
uint32_t PageAllocFreeCount(void);

#endif
