/* SPDX-License-Identifier: BSD-3-Clause */
// Stage2's fixed higher-half memory layout. Physical addresses stay physical
// in page-table entries and frame-allocator APIs; convert only for CPU access.
#ifndef MEMLAYOUT_H
#define MEMLAYOUT_H

#include <lib/stdint.h>

#define DirectMapBase  0xFFFF800000000000ULL
#define KernelVirtBase 0xFFFFFFFF80000000ULL
#define DirectMapLimit (64ULL * 1024ULL * 1024ULL * 1024ULL)

#ifdef __cplusplus
extern "C" {
#endif

// Phys must be backed by the stage2 direct map. Physical zero maps to its
// non-null virtual alias. These helpers do not validate or create mappings.
void *PhysToVirt(uint64_t Phys);
// Virt must belong to the direct map, not the kernel or another virtual alias.
uint64_t VirtToPhys(const void *Virt);

#ifdef __cplusplus
}
#endif

#endif
