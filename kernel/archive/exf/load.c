/* SPDX-License-Identifier: BSD-3-Clause */
// EXF image loader. ExfLoad validates the image in memory, then asks the
// caller-supplied Map hook for a destination for every loadable segment,
// copies the segment's file bytes there and zero-fills the rest of MemSize.
// The freestanding kernel has no memcpy/memset in libk (only stdio.c), so
// the two byte movers below are private to this translation unit. There is
// no dependency on libc or dynamic memory.
#include "exf.h"

// Copy Length bytes from Source to Destination. Byte-wise and direction
// agnostic: segment copies come from the image into freshly mapped memory,
// so the regions never overlap.
static void MemCopy(void* Destination, const void* Source, uint64_t Length)
{
    uint8_t* destination = (uint8_t*)Destination;
    const uint8_t* source = (const uint8_t*)Source;

    for (uint64_t index = 0; index < Length; index++)
        destination[index] = source[index];
}

// Fill Length bytes of Destination with Value, used to zero the MemSize -
// FileSize tail of a segment (the .bss-style part that lives only in memory).
static void MemSet(void* Destination, uint8_t Value, uint64_t Length)
{
    uint8_t* destination = (uint8_t*)Destination;

    for (uint64_t index = 0; index < Length; index++)
        destination[index] = Value;
}

int ExfLoad(const void* Image, uint64_t Size, ExfMapFn Map, void* Context, uint64_t* EntryOut)
{
    if (Map == NULL)
        return ErrIo;

    int status = ExfValidate(Image, Size);
    if (status != ErrOk)
        return status;

    const ExfHeader* header = ExfGetHeader(Image);
    const uint8_t* base = (const uint8_t*)Image;

    for (uint32_t index = 0; index < header->SegmentCount; index++)
    {
        const ExfSegment* segment = ExfSegmentAt(Image, index);
        if (segment == NULL || segment->Type != ExfSegmentTypeLoad)
            continue;

        // The hook maps [VirtAddr, VirtAddr + MemSize) and hands back the
        // destination to fill; failing to map is an out-of-memory condition.
        void* destination = Map(Context, segment);
        if (destination == NULL)
            return ErrNoMem;

        MemCopy(destination, base + segment->FileOffset, segment->FileSize);
        MemSet((uint8_t*)destination + segment->FileSize, 0,
               (uint64_t)segment->MemSize - segment->FileSize);
    }

    if (EntryOut != NULL)
        *EntryOut = header->Entry;

    return ErrOk;
}
