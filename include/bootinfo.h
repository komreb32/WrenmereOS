/* SPDX-License-Identifier: BSD-3-Clause */
// Boot handoff block: stage2 fills it at BootInfoPhys and passes the
// physical address to KernelEntry in rdi (SysV first argument).
#ifndef BOOTINFO_H
#define BOOTINFO_H

// stage2 builds with -Iboot/include (BOOT_STDINT_H), the kernel and
// check-headers with -Iinclude; the quoted path resolves next to this file.
#ifndef BOOT_STDINT_H
#include "lib/stdint.h"
#endif

#define BootInfoMagic   0x57524E4Du // 'M','N','R','W' in memory
#define BootInfoVersion 1u
#define BootInfoPhys    0x00004000u
#define BootInfoMaxSize 256u

typedef struct __attribute__((packed)) BootInfo
{
    uint32_t Magic;              // BootInfoMagic
    uint16_t Version;            // BootInfoVersion
    uint16_t Size;               // sizeof(BootInfo)
    uint64_t MemMapPhys;         // E820 entries (24 bytes each)
    uint32_t MemMapCount;        // E820 entry count
    uint64_t RsdpPhys;           // ACPI RSDP, 0 if none found
    uint64_t FramebufferPhys;    // linear framebuffer, 0 in text mode
    uint32_t FramebufferWidth;
    uint32_t FramebufferHeight;
    uint32_t FramebufferPitch;   // bytes per scanline
    uint32_t FramebufferBpp;
    uint64_t CmdLinePhys;        // NUL-terminated command line, 0 if none
    uint32_t BootDrive;          // BIOS boot drive (DL at stage2 entry)
    uint64_t KernelPhysStart;    // minimum PhysAddr of the loaded segments
    uint64_t KernelPhysEnd;      // maximum PhysAddr of the loaded segments
    uint64_t InitrdPhys;         // 0 when no initrd is loaded
    uint64_t InitrdSize;
} BootInfo;

_Static_assert(sizeof(BootInfo) <= BootInfoMaxSize, "BootInfo exceeds 256 bytes");

#endif
