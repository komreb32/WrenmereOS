/* SPDX-License-Identifier: BSD-3-Clause */
// CRC32 (IEEE 802.3) for EXF image checksums. The reflected polynomial
// 0xEDB88320 is driven through a 256-entry lookup table that is computed the
// first time a CRC is requested, matching the value zlib.crc32 produces and
// therefore the Checksum field written by tools/elf2exf.py.
#include "exf.h"

// Reflected CRC-32 polynomial without the implicit high bit.
#define Crc32Polynomial 0xEDB88320u
// Number of entries in the byte lookup table.
#define Crc32TableSize  256u
// CRC register seeded with all ones, per the IEEE 802.3 definition.
#define Crc32Seed       0xFFFFFFFFu

static uint32_t Crc32Table[Crc32TableSize];
static int Crc32TableReady = 0;

static void Crc32BuildTable(void)
{
    for (uint32_t index = 0; index < Crc32TableSize; index++)
    {
        uint32_t value = index;
        for (int bit = 0; bit < 8; bit++)
        {
            if (value & 1u)
                value = Crc32Polynomial ^ (value >> 1);
            else
                value >>= 1;
        }
        Crc32Table[index] = value;
    }

    Crc32TableReady = 1;
}

uint32_t Crc32Update(uint32_t Crc, const void* Data, uint64_t Length)
{
    if (!Crc32TableReady)
        Crc32BuildTable();

    const uint8_t* bytes = (const uint8_t*)Data;
    for (uint64_t i = 0; i < Length; i++)
        Crc = Crc32Table[(Crc ^ bytes[i]) & 0xFFu] ^ (Crc >> 8);

    return Crc;
}

uint32_t Crc32(const void* Data, uint64_t Length)
{
    return Crc32Update(Crc32Seed, Data, Length) ^ Crc32Seed;
}
