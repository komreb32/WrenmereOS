/* SPDX-License-Identifier: BSD-3-Clause */
// EXF symbol-table lookups. The table is sorted ascending by Value, so
// address lookups use a binary search narrowed to the symbol that starts at
// or before the address, while name lookups walk the table linearly. Both
// re-validate bounds against the header before touching the string table, so
// they are safe to call on any image pointer, even an unvalidated one. There
// is no dependency on libc or dynamic memory.
#include "exf.h"

// Compare two NUL-terminated strings for equality without libc. The kernel
// freestanding environment provides no strcmp, so the loop lives here.
static int ExfStringEqual(const char* Left, const char* Right)
{
    uint64_t index = 0;

    while (Left[index] != '\0' && Left[index] == Right[index])
        index++;

    return Left[index] == Right[index];
}

const ExfSymbol* ExfFindSymbolByAddr(const void* Image, uint64_t Addr, uint64_t* Offset)
{
    const ExfHeader* header = ExfGetHeader(Image);
    if (header == NULL || header->SymbolCount == 0)
        return NULL;

    const ExfSymbol* symbols = (const ExfSymbol*)((const uint8_t*)Image + header->SymbolOffset);

    // Binary search for the first symbol whose Value is past Addr; the
    // candidate is the one right before it (the last symbol starting at or
    // below Addr).
    uint32_t low = 0;
    uint32_t high = header->SymbolCount;
    while (low < high)
    {
        uint32_t middle = low + ((high - low) >> 1);
        if (symbols[middle].Value <= Addr)
            low = middle + 1;
        else
            high = middle;
    }

    if (low == 0)
        return NULL;

    // Walk back from the candidate: normally the single candidate contains
    // Addr, but when symbols share an address or nest the previous entry may
    // be the one whose [Value, Value + Size) range holds Addr. Every entry
    // visited here starts at or below Addr, so Addr - Value never underflows.
    for (uint32_t index = low - 1;; index--)
    {
        const ExfSymbol* symbol = &symbols[index];

        if (Addr - symbol->Value < symbol->Size)
        {
            if (Offset != NULL)
                *Offset = Addr - symbol->Value;
            return symbol;
        }

        if (index == 0)
            break;

        // Stop once no earlier symbol can still reach Addr; a well-formed
        // non-nested table ends the walk right here, keeping this O(log n).
        const ExfSymbol* previous = &symbols[index - 1];
        if (Addr - previous->Value >= previous->Size)
            break;
    }

    return NULL;
}

const ExfSymbol* ExfFindSymbolByName(const void* Image, const char* Name)
{
    const ExfHeader* header = ExfGetHeader(Image);
    if (header == NULL || Name == NULL || header->SymbolCount == 0)
        return NULL;

    const ExfSymbol* symbols = (const ExfSymbol*)((const uint8_t*)Image + header->SymbolOffset);

    for (uint32_t index = 0; index < header->SymbolCount; index++)
    {
        const char* symbolName = ExfSymbolName(Image, &symbols[index]);
        if (symbolName == NULL)
            continue;

        if (ExfStringEqual(symbolName, Name))
            return &symbols[index];
    }

    return NULL;
}

const char* ExfSymbolName(const void* Image, const ExfSymbol* Symbol)
{
    const ExfHeader* header = ExfGetHeader(Image);
    if (header == NULL || Symbol == NULL)
        return NULL;
    if (Symbol->NameOffset >= header->StringSize)
        return NULL;

    const char* name = (const char*)Image + header->StringOffset + Symbol->NameOffset;
    uint64_t remaining = header->StringSize - Symbol->NameOffset;

    // The name must terminate inside the string table, not run past it.
    for (uint64_t index = 0; index < remaining; index++)
    {
        if (name[index] == '\0')
            return name;
    }

    return NULL;
}
