/* SPDX-License-Identifier: BSD-3-Clause */
// Rust memory manager glue: one call from KernelEntry brings it up.
#ifndef KERNEL_MM_MM_H
#define KERNEL_MM_MM_H

#include <lib/stdint.h>

int MmInit(void);
void MmProbeFault(void);
uint64_t MmTotalPages(void);
uint64_t MmTotalRamBytes(void);
uint64_t MmUsableRam(void);   // bytes free
uint64_t MmUsedRam(void);     // bytes in use (total usable - free)


#endif
