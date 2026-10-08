/* SPDX-License-Identifier: BSD-3-Clause */
// EXF v1 executable image format (see docs/exf.md). This header describes the
// on-disk layout of an EXF image: a 64-byte header, a table of 40-byte
// loadable segments, the segment data, a table of 16-byte symbols sorted by
// value and finally a NUL-terminated string table. All integers are little
// endian and every offset is a file offset. The API below validates, inspects
// and loads such an image; functions that can fail return the ErrOk/Err*
// codes from errors.h.
#ifndef KERNEL_ARCHIVE_EXF_EXF_H
#define KERNEL_ARCHIVE_EXF_EXF_H

#include <lib/stdint.h>
#include <errors.h>

// Every image starts with this magic: "EXF\0" stored little endian.
#define ExfMagic         0x00465845u
// Format version carried by the header.
#define ExfVersion       1u
// Fixed header size in bytes (and the file offset of the segment table).
#define ExfHeaderSize    64u
// Size of one segment-table entry in bytes.
#define ExfSegmentSize   40u
// Size of one symbol-table entry in bytes.
#define ExfSymbolSize    16u
// Upper bound on the number of loadable segments in one image.
#define ExfMaxSegments   16u
// Architecture identifier for the x86-64 target.
#define ExfArchX86_64    1u

// Header.Kind values: the role the image plays once it is loaded.
#define ExfKindKernel    1u
#define ExfKindProgram   2u
#define ExfKindDriver    3u
#define ExfKindLibrary   4u

// Segment.Type values: how the loader should handle the segment.
#define ExfSegmentTypeLoad 1u

// Segment.Flags bits: the permissions of the loaded segment.
#define ExfSegmentFlagRead    (1u << 0)
#define ExfSegmentFlagWrite   (1u << 1)
#define ExfSegmentFlagExecute (1u << 2)

// The kernel is freestanding (no <stddef.h>), so provide NULL here for this
// module and every translation unit that includes exf.h.
#ifndef NULL
#define NULL ((void*)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Compile-time size check that works in both C and C++ translation units.
#if defined(__cplusplus)
#define ExfStaticAssert(Expr, Message) static_assert((Expr), Message)
#else
#define ExfStaticAssert(Expr, Message) _Static_assert((Expr), Message)
#endif

// 64-byte image header, always at file offset 0.
typedef struct __attribute__((packed)) {
    uint32_t Magic;         // ExfMagic
    uint16_t Version;       // ExfVersion
    uint16_t HeaderSize;    // ExfHeaderSize (64)
    uint8_t  Kind;          // ExfKind*
    uint8_t  Arch;          // ExfArchX86_64
    uint16_t Flags;         // reserved, zero
    uint16_t SegmentCount;  // number of segments, at most ExfMaxSegments
    uint16_t SegmentSize;   // ExfSegmentSize (40)
    uint64_t Entry;         // virtual address to begin execution at
    uint32_t SegmentOffset; // file offset of the segment table
    uint32_t SymbolCount;   // number of symbols
    uint32_t SymbolOffset;  // file offset of the symbol table
    uint32_t StringOffset;  // file offset of the string table
    uint32_t StringSize;    // size of the string table in bytes
    uint32_t FileSize;      // total size of the image in bytes
    uint32_t Checksum;      // CRC32 (IEEE) of the whole file, this field zero
    uint8_t  Reserved[12];  // reserved, zero
} ExfHeader;

// 40-byte segment-table entry. Segment data is 16-byte aligned in the file.
typedef struct __attribute__((packed)) {
    uint32_t Type;       // ExfSegmentTypeLoad
    uint32_t Flags;      // ExfSegmentFlag* bits
    uint64_t FileOffset; // start of the segment data, 16-byte aligned
    uint64_t VirtAddr;   // virtual address to map the segment at
    uint64_t PhysAddr;   // physical address, used only when Kind is kernel
    uint32_t FileSize;   // bytes stored in the file
    uint32_t MemSize;    // bytes mapped, >= FileSize, the excess is zeroed
} ExfSegment;

// 16-byte symbol-table entry. The table is sorted ascending by Value.
typedef struct __attribute__((packed)) {
    uint64_t Value;      // virtual address of the symbol
    uint32_t Size;       // size of the symbol in bytes
    uint32_t NameOffset; // offset of the NUL-terminated name in the string table
} ExfSymbol;

ExfStaticAssert(sizeof(ExfHeader) == ExfHeaderSize, "ExfHeader must be 64 bytes");
ExfStaticAssert(sizeof(ExfSegment) == ExfSegmentSize, "ExfSegment must be 40 bytes");
ExfStaticAssert(sizeof(ExfSymbol) == ExfSymbolSize, "ExfSymbol must be 16 bytes");

// Caller-supplied mapping hook invoked once per loadable segment. It must make
// [Segment->VirtAddr, Segment->VirtAddr + Segment->MemSize) available and
// return an opaque handle, or NULL when the mapping fails. The Data pointer
// for the segment is the image base plus Segment->FileOffset, so the hook only
// needs to map and zero-fill; ExfLoad copies the file bytes into place.
typedef void* (*ExfMapFn)(void* Context, const ExfSegment* Segment);

// Validate an image held in memory at Image for Size bytes: magic, version,
// header/segment/symbol sizes, table offsets and the trailing CRC32. Returns
// ErrOk when the image is well formed or an errors.h code otherwise.
int ExfValidate(const void* Image, uint64_t Size);

// Return the image header, or NULL when Image is NULL.
const ExfHeader* ExfGetHeader(const void* Image);

// Return the segment at Index, or NULL when Index is past SegmentCount.
const ExfSegment* ExfSegmentAt(const void* Image, uint32_t Index);

// Return the virtual entry address, or 0 when Image is NULL.
uint64_t ExfEntry(const void* Image);

// Return a stable name for a header Kind value: "kernel", "program",
// "driver", "library" or "unknown".
const char* ExfKindName(uint8_t Kind);

// Render a segment Flags word as "RWX", a '-' standing in for a missing flag.
// The result points at a static buffer overwritten by the next call.
const char* ExfSegmentFlagsString(uint32_t Flags);

// Find the symbol whose [Value, Value + Size) range contains Addr. On success
// writes Addr - Value to *Offset (when Offset is not NULL) and returns the
// symbol, otherwise returns NULL.
const ExfSymbol* ExfFindSymbolByAddr(const void* Image, uint64_t Addr, uint64_t* Offset);

// Find the symbol whose name equals Name, or NULL when there is no match.
const ExfSymbol* ExfFindSymbolByName(const void* Image, const char* Name);

// Return the NUL-terminated name of Symbol inside the image string table.
const char* ExfSymbolName(const void* Image, const ExfSymbol* Symbol);

// Load the image through Map: validate it, map each segment and copy its file
// bytes in. On success writes the entry address to *EntryOut (when not NULL)
// and returns ErrOk, otherwise returns an errors.h code and leaves no
// guarantees about partial mappings.
int ExfLoad(const void* Image, uint64_t Size, ExfMapFn Map, void* Context, uint64_t* EntryOut);

// CRC32 (IEEE 802.3, reflected polynomial 0xEDB88320) used by the image
// checksum. Crc32Update folds Data into a running CRC; Crc32 computes the
// CRC32 of a whole buffer (matches zlib.crc32). The 256-entry table is built
// on first use.
uint32_t Crc32Update(uint32_t Crc, const void* Data, uint64_t Length);
uint32_t Crc32(const void* Data, uint64_t Length);

#ifdef __cplusplus
}
#endif

#endif
