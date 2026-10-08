/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdint.h>
#include "../../include/bootinfo.h"

#define VideoMemory   ((volatile uint16_t *)(uintptr_t)0xB8000)
#define ScreenWidth   80
#define ScreenHeight  25
#define TextAttribute 0x07 // light grey on black

#define KernelImageBase ((const uint8_t *)(uintptr_t)0x400000)
#define KernelLoadMin   ((uint64_t)0x100000)
#define KernelLoadMax   ((uint64_t)0x400000)

#define ExfMagic        0x00465845u // "EXF\0"
#define ExfVersion      1u
#define ExfHeaderSize   64u
#define ExfArchX86_64   1u
#define ExfKindKernel   1u
#define ExfSegmentSize  40u
#define ExfSegmentMax   16u
#define ExfTypeLoad     1u
#define ExfChecksumOff  48u

// Fresh tables live in [0x10000, 0x80000), after the BIOS disk buffer is
// no longer needed. The kernel reserves the first MiB, including this area.
#define Pml4Phys            ((uint64_t)0x10000)
#define IdentityPdptPhys    ((uint64_t)0x11000)
#define IdentityPdPhys      ((uint64_t)0x12000)
#define KernelPdptPhys      ((uint64_t)0x13000)
#define DirectMapPdptPhys   ((uint64_t)0x14000)
#define DirectMapPdPhys     ((uint64_t)0x15000)
#define PageTablesEnd       ((uint64_t)0x80000)
#define DirectMapSlot       256u // PML4 index of FFFF800000000000
#define KernelMapSlot       511u
#define KernelPdptSlot      510u
#define DirectMapVirtBase   0xFFFF800000000000ull
#define KernelMapVirtBase   0xFFFFFFFF80000000ull
#define PageBytes           ((uint64_t)0x1000)
#define LargePageBytes      ((uint64_t)0x200000)
#define GiB                 ((uint64_t)0x40000000)
#define DirectMapMaxBytes   (64ull * GiB)
#define PtePresentWrite     0x3ull
#define PteLarge2MB         0x83ull // present | writable | page size
#define E820MapAddr         ((uintptr_t)0x5000)
#define E820CountAddr       ((uintptr_t)0x4FF0)

// Append mapping metadata without changing include/bootinfo.h. Keep the
// legacy Size: the current kernel checks it for equality with sizeof(BootInfo).
// The shared header must expose these fields before the kernel can use them.
typedef struct __attribute__((packed)) Stage2BootInfo
{
    BootInfo Legacy;
    uint64_t DirectMapBase;
    uint64_t KernelVirtBase;
} Stage2BootInfo;

_Static_assert(sizeof(Stage2BootInfo) <= BootInfoMaxSize,
               "Stage2BootInfo exceeds 256 bytes");

extern uint32_t KernelFileSize; // set by LoadKernel in stage2.S (low 32 bits)
extern uint8_t BootDrive;       // BIOS drive number saved from DL (stage2.S)

uint64_t KernelPhysStart; // minimum PhysAddr of the loaded segments
uint64_t KernelPhysEnd;   // maximum PhysAddr of the loaded segments

void ClearScreen(void)
{
    volatile uint16_t *video = VideoMemory;
    int i;

    for (i = 0; i < ScreenWidth * ScreenHeight; i++)
        video[i] = (TextAttribute << 8) | ' ';
}

void PrintString(int x, int y, const char *text)
{
    volatile uint16_t *video = VideoMemory;
    int i;

    if (x < 0 || y < 0)
        return;

    for (i = 0; text[i] != '\0'; i++) {
        if (x >= ScreenWidth || y >= ScreenHeight)
            return;
        video[y * ScreenWidth + x] = (TextAttribute << 8) | (uint8_t)text[i];
        x++;
    }
}

static void Fail(const char *message)
{
    PrintString(0, 0, message);
    for (;;)
        __asm__ volatile ("hlt");
}

static void MemCopy(void *dst, const void *src, uint64_t n)
{
    __asm__ volatile ("rep movsb"
                      : "+D" (dst), "+S" (src), "+c" (n)
                      :: "memory");
}

static void MemSet(void *dst, uint8_t value, uint64_t n)
{
    __asm__ volatile ("rep stosb"
                      : "+D" (dst), "+c" (n)
                      : "a" (value)
                      : "memory");
}

static uint16_t Rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t Rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t Rd64(const uint8_t *p)
{
    return (uint64_t)Rd32(p) |
           ((uint64_t)Rd32(p + 4) << 32);
}

static uint32_t Crc32File(const uint8_t *data, uint32_t size);

static int ExfCheck(const uint8_t *image, uint32_t size)
{
    uint32_t segment_count;
    uint32_t segment_size;
    uint32_t i;
    uint64_t segment_offset;
    uint32_t file_size;
    uint64_t table_end;

    if (size < ExfHeaderSize)
        return 0;
    if (Rd16(image + 6) != ExfHeaderSize)
        return 0;
    if (image[8] != ExfKindKernel)
        return 0;
    if (image[9] != ExfArchX86_64)
        return 0;
    segment_count = Rd16(image + 12);
    segment_size = Rd16(image + 14);
    if (segment_count == 0 || segment_count > ExfSegmentMax)
        return 0;
    if (segment_size != ExfSegmentSize)
        return 0;
    segment_offset = Rd32(image + 24);
    file_size = Rd32(image + 44);
    if (file_size != size)
        return 0;
    table_end = (uint64_t)segment_offset + (uint64_t)segment_count * segment_size;
    if (table_end > size)
        return 0;
    for (i = 0; i < segment_count; i++) {
        const uint8_t *seg = image + segment_offset + (uint64_t)i * segment_size;
        uint64_t file_offset = Rd64(seg + 8);
        uint32_t seg_file_size = Rd32(seg + 32);
        uint32_t seg_mem_size = Rd32(seg + 36);
        if (seg_mem_size < seg_file_size)
            return 0;
        if (file_offset > size || seg_file_size > size - (uint32_t)file_offset)
            return 0;
    }
    return Crc32File(image, size) == Rd32(image + ExfChecksumOff);
}

// CRC32 (IEEE, poly 0xEDB88320) of the whole file with the checksum field
// at ExfChecksumOff treated as zero, matching tools/elf2exf.py.
static uint32_t Crc32File(const uint8_t *data, uint32_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;

    for (i = 0; i < size; i++) {
        uint8_t byte = (i >= ExfChecksumOff && i < ExfChecksumOff + 4)
                           ? 0
                           : data[i];
        int bit;

        crc ^= byte;
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int)(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint64_t ExfLoadSegments(const uint8_t *image, uint32_t size)
{
    uint32_t segment_count = Rd16(image + 12);
    uint32_t segment_offset = Rd32(image + 24);
    uint64_t entry = Rd64(image + 16);
    uint64_t start = UINT64_MAX;
    uint64_t end = 0;
    uint32_t i;

    for (i = 0; i < segment_count; i++) {
        const uint8_t *seg = image + segment_offset + (uint64_t)i * ExfSegmentSize;
        uint32_t type = Rd32(seg + 0);
        uint64_t file_offset = Rd64(seg + 8);
        uint64_t phys = Rd64(seg + 24);
        uint32_t seg_file_size = Rd32(seg + 32);
        uint32_t seg_mem_size = Rd32(seg + 36);

        if (type != ExfTypeLoad)
            return 0;
        if (seg_mem_size < seg_file_size)
            return 0;
        if (file_offset > size || seg_file_size > size - (uint32_t)file_offset)
            return 0;
        if (phys < KernelLoadMin)
            return 0;
        if ((uint64_t)seg_mem_size > KernelLoadMax - phys)
            return 0;
        MemCopy((void *)(uintptr_t)phys, image + file_offset, seg_file_size);
        if (seg_mem_size > seg_file_size)
            MemSet((void *)(uintptr_t)(phys + seg_file_size), 0,
                   (uint64_t)seg_mem_size - seg_file_size);
        if (phys < start)
            start = phys;
        if (phys > end)
            end = phys;
    }
    if (start > end)
        return 0;
    KernelPhysStart = start;
    KernelPhysEnd = end;
    return entry;
}

static int BuildPageTables(void)
{
    uint64_t ram_end = 0;
    uint64_t tables_end;
    volatile uint64_t *pml4 = (volatile uint64_t *)(uintptr_t)Pml4Phys;
    volatile uint64_t *identity_pdpt =
        (volatile uint64_t *)(uintptr_t)IdentityPdptPhys;
    volatile uint64_t *identity_pd =
        (volatile uint64_t *)(uintptr_t)IdentityPdPhys;
    volatile uint64_t *direct_pdpt =
        (volatile uint64_t *)(uintptr_t)DirectMapPdptPhys;
    volatile uint64_t *kernel_pdpt =
        (volatile uint64_t *)(uintptr_t)KernelPdptPhys;
    uint32_t pd_count;
    uint16_t e820_count = *(volatile uint16_t *)E820CountAddr;
    uint32_t i;
    uint32_t j;

    // DetectMemory stores up to 32 usable E820 entries. Use their highest
    // physical end, not the sum of lengths (RAM can have holes above 4 GiB).
    if (e820_count > 32)
        return 0;
    for (i = 0; i < e820_count; i++) {
        const uint8_t *entry = (const uint8_t *)(E820MapAddr + (uint64_t)i * 24);
        uint64_t base = Rd64(entry);
        uint64_t length = Rd64(entry + 8);

        if (Rd32(entry + 16) != 1)
            continue;
        if (length != 0 && base <= UINT64_MAX - length && base + length > ram_end)
            ram_end = base + length;
    }

    if (ram_end == 0)
        ram_end = GiB; // E820 unavailable: keep one GiB mapped
    if (ram_end > DirectMapMaxBytes)
        ram_end = DirectMapMaxBytes;
    pd_count = (uint32_t)((ram_end + GiB - 1) / GiB);
    tables_end = DirectMapPdPhys + (uint64_t)pd_count * PageBytes;
    if (tables_end > PageTablesEnd)
        return 0;

    MemSet((void *)(uintptr_t)Pml4Phys, 0, tables_end - Pml4Phys);
    // Keep a minimal identity map only for the first 2 MiB, which covers:
    // - Stage2 code/data/stack (0x8000-0x9fff)
    // - BootInfo (0x4000) and E820 map (0x5000)
    // - Early page tables (0x10000-0x80000)
    // - VGA framebuffer alias (0xb8000) used by the kernel's stdio.c
    // This keeps the transition safe and the kernel console working without
    // exposing all low memory via identity map.
    pml4[0] = IdentityPdptPhys | PtePresentWrite;
    identity_pdpt[0] = IdentityPdPhys | PtePresentWrite;
    identity_pd[0] = 0 * LargePageBytes | PteLarge2MB;

    pml4[DirectMapSlot] = DirectMapPdptPhys | PtePresentWrite;
    for (i = 0; i < pd_count; i++) {
        uint64_t pd_phys = DirectMapPdPhys + (uint64_t)i * PageBytes;
        volatile uint64_t *pd = (volatile uint64_t *)(uintptr_t)pd_phys;

        direct_pdpt[i] = pd_phys | PtePresentWrite;
        for (j = 0; j < 512; j++) {
            uint64_t phys = (uint64_t)i * GiB + (uint64_t)j * LargePageBytes;

            if (phys >= ram_end)
                break;
            pd[j] = phys | PteLarge2MB;
        }
    }
    // Higher-half kernel alias at 0xFFFFFFFF80000000: the kernel links
    // there and stage2.S jumps to its virtual entry after we return.
    // PML4 slot 511 -> KernelPdpt; PDPT slot 510 shares the first
    // direct-map page directory, whose 2 MiB pages map physical frames
    // at the same offsets, so KernelVirtBase + phys resolves to phys.
    pml4[KernelMapSlot] = KernelPdptPhys | PtePresentWrite;
    kernel_pdpt[KernelPdptSlot] = DirectMapPdPhys | PtePresentWrite;
    // The new identity map keeps stage2, its stack and BootInfo accessible
    // while CR3 changes; Entry already names the higher-half kernel alias.
    __asm__ volatile ("mov %0, %%cr3" :: "r" (Pml4Phys) : "memory");
    return 1;
}

static int RsdpValid(const uint8_t *p)
{
    uint8_t sum = 0;
    int i;

    if (Rd64(p) != 0x2052545020445352ull) // "RSD PTR " little-endian
        return 0;
    for (i = 0; i < 20; i++)
        sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

static uint64_t FindRsdp(void)
{
    uint32_t ebda_seg;
    uint64_t ebda;
    uint64_t addr;

    // BDA word at 0x40E holds the EBDA base segment (read with asm so the
    // fixed hardware address is not treated as an array access).
    __asm__ volatile ("movzwl 0x40E, %0" : "=r" (ebda_seg));
    ebda = (uint64_t)ebda_seg << 4;

    if (ebda != 0) {
        for (addr = (ebda + 15) & ~15ull; addr + 20 <= ebda + 1024; addr += 16)
            if (RsdpValid((const uint8_t *)(uintptr_t)addr))
                return addr;
    }
    for (addr = 0xE0000; addr + 20 <= 0x100000; addr += 16)
        if (RsdpValid((const uint8_t *)(uintptr_t)addr))
            return addr;
    return 0;
}

static void BuildBootInfo(void)
{
    volatile Stage2BootInfo *extended =
        (volatile Stage2BootInfo *)(uintptr_t)BootInfoPhys;
    volatile BootInfo *info = &extended->Legacy;

    MemSet((void *)(uintptr_t)BootInfoPhys, 0, sizeof(Stage2BootInfo));
    info->Magic = BootInfoMagic;
    info->Version = BootInfoVersion;
    info->Size = (uint16_t)sizeof(BootInfo);
    info->MemMapPhys = E820MapAddr;
    info->MemMapCount = (uint32_t)*(volatile uint16_t *)E820CountAddr;
    info->RsdpPhys = FindRsdp();
    info->BootDrive = BootDrive;
    info->KernelPhysStart = KernelPhysStart;
    info->KernelPhysEnd = KernelPhysEnd;
    extended->DirectMapBase = DirectMapVirtBase;
    extended->KernelVirtBase = KernelMapVirtBase;
}

uint64_t Stage2Main(void)
{
    const uint8_t *image = KernelImageBase;
    uint32_t size = KernelFileSize;
    uint64_t entry;

    ClearScreen();
    if (!ExfCheck(image, size))
        Fail("EXF CHECK");
    entry = ExfLoadSegments(image, size);
    if (entry == 0)
        Fail("EXF LOAD");
    if (!BuildPageTables())
        Fail("EXF MAP");
    BuildBootInfo();
    return entry;
}