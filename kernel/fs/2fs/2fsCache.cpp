/* SPDX-License-Identifier: BSD-3-Clause */
// 2fs block cache: a static pool of CacheBlockCount 4096-byte buffers,
// 16-byte aligned, that hides the device behind GetBlock/PutBlock and writes
// data back with MarkDirty/FlushBlock/FlushAll. Blocks are volume-relative
// and reach the disk as LBA = StartLba + Block * 8 (8 sectors per block).
// Replacement is LRU with a monotonic stamp advanced by GetBlock and
// NewBlock, and it never evicts a pinned slot (Use > 0); a dirty victim is
// written back before its slot is reused. No dynamic memory is used and
// every entry point returns an errors.h code.
#include "2fsLayout.h"

namespace Fs2 {
namespace {

// One slot of the pool: which block it holds, when it was touched last, how
// many callers pin it and whether its content is meaningful / behind disk.
struct CacheEntry {
    uint64_t Block;  // volume block in this slot, meaningful when Valid
    uint64_t Stamp;  // monotonic tick of the last access, drives LRU
    uint32_t Use;    // pin count; > 0 blocks eviction
    bool     Valid;
    bool     Dirty;
};

alignas(CacheAlignment) uint8_t CacheData[CacheBlockCount][BlockSize];
CacheEntry   CacheTable[CacheBlockCount];
uint64_t     CacheTick     = 0;
BlockDevice* CacheDevice   = nullptr;
uint64_t     CacheStartLba = 0;

// Byte-wise clear: freestanding code cannot rely on a hosted memset.
void ZeroBytes(uint8_t* Destination, uint64_t Length)
{
    for (uint64_t index = 0; index < Length; index++)
        Destination[index] = 0;
}

// Index of the slot holding Block, or -1 when it is not resident.
int FindSlot(uint64_t Block)
{
    for (uint32_t index = 0; index < CacheBlockCount; index++)
        if (CacheTable[index].Valid && CacheTable[index].Block == Block)
            return (int)index;

    return -1;
}

void Touch(CacheEntry* Entry)
{
    Entry->Stamp = ++CacheTick;
}

int DiskRead(uint64_t Block, uint8_t* Destination)
{
    if (CacheDevice == nullptr)
        return ErrIo;

    return CacheDevice->Read(CacheDevice,
                             CacheStartLba + Block * SectorsPerBlock,
                             SectorsPerBlock, Destination);
}

int DiskWrite(uint64_t Block, const uint8_t* Source)
{
    if (CacheDevice == nullptr)
        return ErrIo;

    return CacheDevice->Write(CacheDevice,
                              CacheStartLba + Block * SectorsPerBlock,
                              SectorsPerBlock, Source);
}

int DiskFlush(void)
{
    if (CacheDevice == nullptr)
        return ErrIo;

    return CacheDevice->Flush(CacheDevice);
}

// Write one slot back; a failure leaves it dirty for a later retry.
int WriteSlot(uint32_t Index)
{
    CacheEntry* entry = &CacheTable[Index];
    if (!entry->Dirty)
        return ErrOk;

    int status = DiskWrite(entry->Block, CacheData[Index]);
    if (status == ErrOk)
        entry->Dirty = false;

    return status;
}

// Pick a slot for a new block: any empty one, else the least recently used
// unpinned one, writing it back first when it is dirty. Every slot pinned
// means there is nothing to spare.
int AcquireSlot(int* Out)
{
    for (uint32_t index = 0; index < CacheBlockCount; index++)
    {
        if (!CacheTable[index].Valid)
        {
            *Out = (int)index;
            return ErrOk;
        }
    }

    int oldest = -1;
    for (uint32_t index = 0; index < CacheBlockCount; index++)
    {
        const CacheEntry* entry = &CacheTable[index];
        if (entry->Use > 0)
            continue;
        if (oldest < 0 || entry->Stamp < CacheTable[oldest].Stamp)
            oldest = (int)index;
    }

    if (oldest < 0)
        return ErrNoMem;

    int status = WriteSlot((uint32_t)oldest);
    if (status != ErrOk)
        return status;

    *Out = oldest;
    return ErrOk;
}

// Forget every slot without touching the device.
void DropAll(void)
{
    for (uint32_t index = 0; index < CacheBlockCount; index++)
    {
        CacheTable[index].Block = 0;
        CacheTable[index].Stamp = 0;
        CacheTable[index].Use = 0;
        CacheTable[index].Valid = false;
        CacheTable[index].Dirty = false;
    }
}

} // namespace

int CacheBind(BlockDevice* Device, uint64_t StartLba)
{
    if (Device == nullptr)
        return ErrIo;

    // Serving stale buffers for a new device would be silent corruption, so
    // refuse to rebind while something still owes the current device a write.
    for (uint32_t index = 0; index < CacheBlockCount; index++)
        if (CacheTable[index].Valid && CacheTable[index].Dirty)
            return ErrIo;

    DropAll();
    CacheDevice = Device;
    CacheStartLba = StartLba;
    return ErrOk;
}

int CacheUnbind(void)
{
    int status = FlushAll();
    if (status != ErrOk)
        return status; // keep the binding so the caller can retry

    DropAll();
    CacheDevice = nullptr;
    CacheStartLba = 0;
    return ErrOk;
}

int GetBlock(uint64_t Block, uint8_t** BufferOut)
{
    if (BufferOut == nullptr)
        return ErrIo;

    int slot = FindSlot(Block);
    if (slot >= 0)
    {
        CacheTable[slot].Use++;
        Touch(&CacheTable[slot]);
        *BufferOut = CacheData[slot];
        return ErrOk;
    }

    int index = 0;
    int status = AcquireSlot(&index);
    if (status != ErrOk)
        return status;

    status = DiskRead(Block, CacheData[index]);
    if (status != ErrOk)
        return status; // the slot keeps its previous, consistent content

    CacheTable[index].Block = Block;
    CacheTable[index].Use = 1;
    CacheTable[index].Valid = true;
    CacheTable[index].Dirty = false;
    Touch(&CacheTable[index]);
    *BufferOut = CacheData[index];
    return ErrOk;
}

int MarkDirty(uint64_t Block)
{
    int slot = FindSlot(Block);
    if (slot < 0)
        return ErrIo;

    // No Touch here: the stamp tracks buffer fetches (GetBlock/NewBlock), so
    // marking dirty does not push the block to the back of the LRU queue.
    CacheTable[slot].Dirty = true;
    return ErrOk;
}

int PutBlock(uint64_t Block)
{
    int slot = FindSlot(Block);
    if (slot < 0 || CacheTable[slot].Use == 0)
        return ErrIo; // not resident, or more puts than gets

    CacheTable[slot].Use--;
    return ErrOk;
}

int FlushBlock(uint64_t Block)
{
    int slot = FindSlot(Block);
    if (slot < 0)
        return ErrOk; // nothing resident, nothing to write

    return WriteSlot((uint32_t)slot);
}

int FlushAll(void)
{
    int status = ErrOk;

    for (uint32_t index = 0; index < CacheBlockCount; index++)
    {
        if (!CacheTable[index].Valid || !CacheTable[index].Dirty)
            continue;

        int written = WriteSlot(index);
        if (written != ErrOk && status == ErrOk)
            status = written; // keep going so one bad block loses the rest
    }

    if (status == ErrOk)
        status = DiskFlush();

    return status;
}

int ZeroBlock(uint64_t Block)
{
    int slot = FindSlot(Block);
    if (slot < 0)
        return ErrIo; // the caller must hold the block (GetBlock/NewBlock)

    ZeroBytes(CacheData[slot], BlockSize);
    CacheTable[slot].Dirty = true;
    return ErrOk;
}

int NewBlock(uint64_t Block, uint8_t** BufferOut)
{
    if (BufferOut == nullptr)
        return ErrIo;

    int slot = FindSlot(Block);
    if (slot < 0)
    {
        // A brand-new block has no meaningful content on disk, so the slot
        // is taken (writing a dirty victim out) but never read from.
        int status = AcquireSlot(&slot);
        if (status != ErrOk)
            return status;

        CacheTable[slot].Block = Block;
        CacheTable[slot].Use = 1;
        CacheTable[slot].Valid = true;
        CacheTable[slot].Dirty = true;
        Touch(&CacheTable[slot]);
    }
    else
    {
        CacheTable[slot].Use++;
        CacheTable[slot].Dirty = true;
        Touch(&CacheTable[slot]);
    }

    ZeroBytes(CacheData[slot], BlockSize);
    *BufferOut = CacheData[slot];
    return ErrOk;
}

} // namespace Fs2
