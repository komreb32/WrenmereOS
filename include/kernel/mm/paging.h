/* SPDX-License-Identifier: BSD-3-Clause */
// Rust paging: 4 KiB OS pages mapped as single hardware entries.
#ifndef KERNEL_MM_PAGING_H
#define KERNEL_MM_PAGING_H

#include <lib/stdint.h>

#define PagingPageSize 4096u
#define PagingPresent  0x001u
#define PagingWritable 0x002u
#define PagingUser     0x004u

// Hooks into the live stage2 tables; new tables come from the frame pool.
int PagingInit(void);
int PagingMap(uint64_t Virt, uint64_t Phys, uint64_t Flags);
int PagingUnmap(uint64_t Virt);
int PagingTranslate(uint64_t Virt, uint64_t *PhysOut);
int PagingIsEnabled(void);

#endif
