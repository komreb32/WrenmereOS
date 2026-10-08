/* SPDX-License-Identifier: BSD-3-Clause */
// EXF v1 image inspection and validation. This translation unit implements the
// small accessors over an in-memory image plus ExfValidate, which checks the
// header, the table geometry, every segment and symbol, and finally the
// trailing CRC32. It has no dependency on libc or dynamic memory.
#include "exf.h"

// Seed for the running CRC32, matching the IEEE 802.3 definition used by
// crc32.c and by tools/elf2exf.py.
#define ExfCrcSeed 0xFFFFFFFFu
// File offset of the CRC32 field inside the header; the four bytes there are
// taken as zero when recomputing the checksum.
#define ExfChecksumOffset 48u
// Length of the checksum field in bytes.
#define ExfChecksumSize 4u

const ExfHeader* ExfGetHeader(const void* Image)
{
    if (Image == NULL)
        return NULL;

    return (const ExfHeader*)Image;
}

const ExfSegment* ExfSegmentAt(const void* Image, uint32_t Index)
{
    const ExfHeader* header = ExfGetHeader(Image);
    if (header == NULL || Index >= header->SegmentCount)
        return NULL;

    const uint8_t* table = (const uint8_t*)Image + header->SegmentOffset;
    return (const ExfSegment*)table + Index;
}

uint64_t ExfEntry(const void* Image)
{
    const ExfHeader* header = ExfGetHeader(Image);
    if (header == NULL)
        return 0;

    return header->Entry;
}

const char* ExfKindName(uint8_t Kind)
{
    switch (Kind)
    {
    case ExfKindKernel:  return "kernel";
    case ExfKindProgram: return "program";
    case ExfKindDriver:  return "driver";
    case ExfKindLibrary: return "library";
    default:             return "unknown";
    }
}

const char* ExfSegmentFlagsString(uint32_t Flags)
{
    static char text[4];

    text[0] = (Flags & ExfSegmentFlagRead) ? 'R' : '-';
    text[1] = (Flags & ExfSegmentFlagWrite) ? 'W' : '-';
    text[2] = (Flags & ExfSegmentFlagExecute) ? 'X' : '-';
    text[3] = '\0';

    return text;
}

// True once every string table entry can be NUL-terminated within the table.
static int ExfCheckStrings(const uint8_t* Base, const ExfHeader* Header)
{
    const char* Strings = (const char*)Base + Header->StringOffset;

    if (Header->StringSize == 0 || Strings[0] != '\0')
        return 0;

    return 1;
}

// Check one loadable segment: type, alignment, bounds, size ordering and flags.
static int ExfCheckSegment(const uint8_t* Base, const ExfHeader* Header,
                           const ExfSegment* Segment)
{
    const uint32_t KnownFlags =
        ExfSegmentFlagRead | ExfSegmentFlagWrite | ExfSegmentFlagExecute;

    if (Segment->Type != ExfSegmentTypeLoad)
        return 0;
    if ((Segment->FileOffset & 0xF) != 0)
        return 0;
    if ((uint64_t)Segment->FileOffset + Segment->FileSize > Header->FileSize)
        return 0;
    if (Segment->MemSize < Segment->FileSize)
        return 0;
    if ((Segment->Flags & ~KnownFlags) != 0)
        return 0;

    return 1;
}

// Check the symbol table: sorted ascending by Value, each name inside the
// string table and NUL-terminated there.
static int ExfCheckSymbols(const uint8_t* Base, const ExfHeader* Header)
{
    const ExfSymbol* Symbols = (const ExfSymbol*)(Base + Header->SymbolOffset);
    uint64_t Previous = 0;

    for (uint32_t index = 0; index < Header->SymbolCount; index++)
    {
        const ExfSymbol* symbol = &Symbols[index];

        if (index > 0 && symbol->Value < Previous)
            return 0;
        Previous = symbol->Value;

        if (symbol->NameOffset >= Header->StringSize)
            return 0;

        const char* name = (const char*)Base + Header->StringOffset + symbol->NameOffset;
        uint64_t remaining = Header->StringSize - symbol->NameOffset;
        uint64_t length = 0;
        while (length < remaining && name[length] != '\0')
            length++;
        if (length == remaining)
            return 0;
    }

    return 1;
}

int ExfValidate(const void* Image, uint64_t Size)
{
    if (Image == NULL || Size < ExfHeaderSize)
        return ErrIo;

    const uint8_t* base = (const uint8_t*)Image;
    const ExfHeader* header = (const ExfHeader*)Image;

    if (header->Magic != ExfMagic)
        return ErrIo;
    if (header->Version != ExfVersion)
        return ErrIo;
    if (header->HeaderSize != ExfHeaderSize)
        return ErrIo;
    if (header->SegmentSize != ExfSegmentSize)
        return ErrIo;
    if (header->Arch != ExfArchX86_64)
        return ErrIo;
    if (header->Kind < ExfKindKernel || header->Kind > ExfKindLibrary)
        return ErrIo;
    if (header->SegmentCount > ExfMaxSegments)
        return ErrIo;
    if (header->FileSize < ExfHeaderSize)
        return ErrIo;
    if ((uint64_t)header->FileSize > Size)
        return ErrIo;

    uint64_t segmentEnd =
        (uint64_t)header->SegmentOffset + (uint64_t)header->SegmentCount * ExfSegmentSize;
    if (header->SegmentOffset < ExfHeaderSize || segmentEnd > header->FileSize)
        return ErrIo;

    uint64_t symbolEnd =
        (uint64_t)header->SymbolOffset + (uint64_t)header->SymbolCount * ExfSymbolSize;
    if (symbolEnd > header->FileSize)
        return ErrIo;

    uint64_t stringEnd = (uint64_t)header->StringOffset + header->StringSize;
    if (stringEnd > header->FileSize)
        return ErrIo;

    if (!ExfCheckStrings(base, header))
        return ErrIo;

    for (uint32_t index = 0; index < header->SegmentCount; index++)
    {
        const ExfSegment* segment = ExfSegmentAt(Image, index);
        if (!ExfCheckSegment(base, header, segment))
            return ErrIo;
    }

    if (!ExfCheckSymbols(base, header))
        return ErrIo;

    // Recompute the CRC32 over the file with the checksum field read as zero:
    // the four bytes there contribute to the CRC as zero bytes.
    static const uint8_t zero[ExfChecksumSize] = { 0, 0, 0, 0 };
    uint32_t crc = Crc32Update(ExfCrcSeed, base, ExfChecksumOffset);
    crc = Crc32Update(crc, zero, ExfChecksumSize);
    crc = Crc32Update(crc, base + ExfChecksumOffset + ExfChecksumSize,
                      header->FileSize - (ExfChecksumOffset + ExfChecksumSize));
    crc ^= ExfCrcSeed;

    if (crc != header->Checksum)
        return ErrIo;

    return ErrOk;
}
