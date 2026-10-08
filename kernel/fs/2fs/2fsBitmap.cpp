/* SPDX-License-Identifier: BSD-3-Clause */
// 2fs block and inode allocators over the on-disk bitmaps (docs/2fs.md):
// AllocBlock, AllocBlocks (scattered or contiguous), FreeBlockRange,
// AllocInode, FreeInode and IsBlockUsed. Bit N names block/inode N
// (byte N/8, bit N%8, LSB first, 1 = used), exactly as tools/2fs.py
// formats them. Every bitmap block travels through the Fs2 block cache,
// allocations only come from past DataStart (DataStart itself holds the
// root directory, the backup copy is reserved), and multi-block updates
// flip the bits first and move the superblock counters second, so a
// mid-update device error never moves a counter without the bits behind
// it. Placement hints (BlockHint/InodeHint) resume each scan where the
// previous allocation stopped. No dynamic memory; every entry point
// returns an errors.h code.
#include "2fsLayout.h"

namespace Fs2 {
namespace {

// Placement hints: where the previous allocation stopped. Only a starting
// point for the next scan - clamped into range, so a stale hint just costs
// a pass over the bitmap, never a wrong answer. Inode 0 is invalid and 1
// is the root, so the inode hint starts at the first user inode.
uint64_t BlockHint = 0;
uint64_t InodeHint = 2;

// Clamp the block hint into the allocatable area: the allocator never hands
// out DataStart or below, so a hint stranded there would scan used bits.
uint64_t BlockHintGet(const Superblock* Super)
{
    uint64_t hint = BlockHint;

    if (hint <= Super->DataStart || hint >= Super->TotalBlocks)
        hint = Super->DataStart + 1;

    return hint;
}

uint64_t InodeHintGet(const Superblock* Super)
{
    uint64_t hint = InodeHint;

    if (hint < 2 || hint >= Super->TotalInodes)
        hint = 2;

    return hint;
}

// Bit N is byte N/8, bit N%8 (LSB first): the buffer must stay pinned
// across the call.
bool BitTest(const uint8_t* Bitmap, uint64_t Bit)
{
    return (Bitmap[Bit / 8] & (uint8_t)(1u << (Bit % 8))) != 0;
}

void BitSet(uint8_t* Bitmap, uint64_t Bit)
{
    Bitmap[Bit / 8] |= (uint8_t)(1u << (Bit % 8));
}

void BitClear(uint8_t* Bitmap, uint64_t Bit)
{
    Bitmap[Bit / 8] &= (uint8_t)~(1u << (Bit % 8));
}

// Bitmap block holding Bit inside [RegionStart, RegionStart+RegionBlocks):
// bit 0 of the region lives in the first block. ErrCorrupt when Bit falls
// outside the region, meaning the caller named an index the volume cannot
// hold.
int BitmapBlockFor(uint64_t RegionStart, uint64_t RegionBlocks, uint64_t Bit,
                   uint64_t* BlockOut)
{
    if (Bit / BitsPerBitmapBlock >= RegionBlocks)
        return ErrCorrupt;

    *BlockOut = RegionStart + Bit / BitsPerBitmapBlock;
    return ErrOk;
}

// Geometry guard for block allocation: the volume is mounted and the data
// area plus the backup copy sit inside it. Mount already validated the
// regions; this re-checks the fields the allocator walks so a corrupted
// in-memory copy reads as ErrCorrupt instead of wandering the disk.
int SuperForBlocks(Superblock** Out)
{
    Superblock* super = SuperGet();
    if (super == nullptr)
        return ErrIo; // no mounted volume behind the allocator

    if (super->TotalBlocks == 0 || super->DataStart < 2 ||
        super->DataStart >= super->TotalBlocks)
        return ErrCorrupt;
    if (super->BackupBlock >= super->TotalBlocks)
        return ErrCorrupt;
    if (super->FreeBlocks > super->TotalBlocks)
        return ErrCorrupt;

    *Out = super;
    return ErrOk;
}

int SuperForInodes(Superblock** Out)
{
    Superblock* super = SuperGet();
    if (super == nullptr)
        return ErrIo;

    if (super->TotalInodes < 2)
        return ErrCorrupt;
    if (super->FreeInodes > super->TotalInodes)
        return ErrCorrupt;

    *Out = super;
    return ErrOk;
}

// Read one bitmap bit through the cache (pin + unpin around the read).
int ReadBit(uint64_t RegionStart, uint64_t RegionBlocks, uint64_t Bit,
            bool* UsedOut)
{
    uint64_t block = 0;
    int status = BitmapBlockFor(RegionStart, RegionBlocks, Bit, &block);
    if (status != ErrOk)
        return status;

    uint8_t* buffer = nullptr;
    status = GetBlock(block, &buffer);
    if (status != ErrOk)
        return status;

    *UsedOut = BitTest(buffer, Bit);

    return PutBlock(block);
}

// First-fit scan of the block bitmap, resuming at BlockHint and wrapping
// inside the data area ([DataStart, TotalBlocks)). Phase one only reads:
// it records the winning run(s) in the caller's RunsOut and returns
// ErrNoMem when Count free blocks - or room to report their runs - do not
// fit. One bitmap block stays pinned while the cursor walks its bits.
int ScanBlocks(Superblock* Super, uint64_t Count, bool Contiguous,
               AllocRun* RunsOut, uint32_t RunsCapacity, uint32_t* RunsUsedOut)
{
    uint64_t first = Super->DataStart + 1; // DataStart holds the root dir
    uint64_t total = Super->TotalBlocks;
    uint64_t span = total - first; // >= 1, SuperForBlocks checked it
    uint64_t cursor = BlockHintGet(Super);

    bool haveResident = false;
    uint64_t resident = 0;
    uint8_t* bits = nullptr;

    uint64_t runStart = 0;
    uint64_t runLength = 0;
    uint32_t runs = 0;
    uint64_t found = 0;
    uint64_t previous = 0;
    bool havePrevious = false;
    int status = ErrNoMem; // until the scan proves the request fits

    for (uint64_t step = 0; step < span; step++)
    {
        uint64_t bit = first + (cursor - first + step) % span;

        if (havePrevious && bit != previous + 1)
            runLength = 0; // wrapped past the end: runs cannot cross it
        havePrevious = true;
        previous = bit;

        uint64_t bitmapBlock = 0;
        int blockStatus = BitmapBlockFor(Super->BlockBitmapStart,
                                         Super->BlockBitmapBlocks,
                                         bit, &bitmapBlock);
        if (blockStatus != ErrOk)
        {
            status = blockStatus;
            break;
        }
        if (!haveResident || bitmapBlock != resident)
        {
            if (haveResident)
            {
                int released = PutBlock(resident);
                haveResident = false;
                if (released != ErrOk)
                {
                    status = released;
                    break;
                }
            }

            uint8_t* buffer = nullptr;
            int got = GetBlock(bitmapBlock, &buffer);
            if (got != ErrOk)
            {
                status = got;
                break;
            }
            resident = bitmapBlock;
            bits = buffer;
            haveResident = true;
        }

        // DataStart (root dir) and the backup copy are never allocatable even
        // if a stray format left their bits clear: only data bits past
        // DataStart are handed out.
        bool allocatable = !BitTest(bits, bit) &&
                           bit != Super->BackupBlock;
        if (!allocatable)
        {
            runLength = 0;
            continue;
        }

        if (runLength == 0)
        {
            if (!Contiguous && runs == RunsCapacity)
            {
                status = ErrNoMem; // nowhere to report another run
                break;
            }
            runStart = bit;
            if (!Contiguous)
            {
                RunsOut[runs].Start = bit;
                RunsOut[runs].Count = 1;
                runs++;
            }
        }
        else if (!Contiguous)
        {
            RunsOut[runs - 1].Count++;
        }
        runLength++;
        found++;

        if (Contiguous && runLength == Count)
        {
            RunsOut[0].Start = runStart;
            RunsOut[0].Count = Count;
            runs = 1;
            status = ErrOk;
            break;
        }
        if (!Contiguous && found == Count)
        {
            status = ErrOk;
            break;
        }
    }

    if (haveResident)
    {
        int released = PutBlock(resident);
        if (status == ErrOk)
            status = released;
    }
    if (RunsUsedOut != nullptr)
        *RunsUsedOut = (status == ErrOk) ? runs : 0;
    return status;
}

// Flip one bit toward Used (1) or free (0) and mark its bitmap block dirty.
// The caller moves the superblock counter once the whole update is done.
int FlipBit(uint64_t RegionStart, uint64_t RegionBlocks, uint64_t Bit,
            bool Used)
{
    uint64_t block = 0;
    int status = BitmapBlockFor(RegionStart, RegionBlocks, Bit, &block);
    if (status != ErrOk)
        return status;

    uint8_t* buffer = nullptr;
    status = GetBlock(block, &buffer);
    if (status != ErrOk)
        return status;

    if (Used)
        BitSet(buffer, Bit);
    else
        BitClear(buffer, Bit);

    status = MarkDirty(block);
    int released = PutBlock(block);

    if (status != ErrOk)
        return status;
    return released;
}

// Phase two: verify every run bit still reads free, then flip it and move
// FreeBlocks once per block. A bit that turned used between scan and commit
// aborts with ErrCorrupt (single-threaded kernel: only on-disk meddling
// explains it); bits flipped before the abort stay allocated and counted,
// so bitmap and counter never disagree.
int CommitRuns(Superblock* Super, const AllocRun* Runs, uint32_t RunCount,
               uint64_t Count)
{
    for (uint32_t run = 0; run < RunCount; run++)
    {
        for (uint64_t index = 0; index < Runs[run].Count; index++)
        {
            uint64_t bit = Runs[run].Start + index;
            bool used = true;
            int status = ReadBit(Super->BlockBitmapStart,
                                 Super->BlockBitmapBlocks, bit, &used);
            if (status != ErrOk)
                return status;
            if (used)
                return ErrCorrupt;

            status = FlipBit(Super->BlockBitmapStart,
                             Super->BlockBitmapBlocks, bit, true);
            if (status != ErrOk)
                return status;

            Super->FreeBlocks--;
            if (--Count == 0)
                return ErrOk;
        }
    }

    return ErrCorrupt; // runs did not cover Count: caller bug, never happens
}

} // namespace

int AllocBlock(uint64_t* BlockOut)
{
    if (BlockOut == nullptr)
        return ErrIo;

    AllocRun run;
    uint32_t used = 0;
    int status = AllocBlocks(1, true, &run, 1, &used);
    if (status != ErrOk)
        return status;

    *BlockOut = run.Start;
    return ErrOk;
}
int AllocBlocks(uint64_t Count, bool Contiguous, AllocRun* RunsOut,
                uint32_t RunsCapacity, uint32_t* RunsUsedOut)
{
    if (Count == 0)
        return ErrIo;
    if (RunsOut == nullptr || RunsCapacity == 0 || RunsUsedOut == nullptr)
        return ErrIo;
    if (Contiguous && RunsCapacity < 1)
        return ErrIo;
    *RunsUsedOut = 0;

    Superblock* super = nullptr;
    int status = SuperForBlocks(&super);
    if (status != ErrOk)
        return status;
    if (Count > super->FreeBlocks)
        return ErrNoMem;

    uint32_t found = 0;
    status = ScanBlocks(super, Count, Contiguous, RunsOut, RunsCapacity,
                        &found);
    if (status != ErrOk)
        return status;

    status = CommitRuns(super, RunsOut, found, Count);
    if (status != ErrOk)
        return status;

    // Advance the hint past the last block handed out; BlockHintGet clamps
    // it back into the data area on the next call.
    BlockHint = RunsOut[found - 1].Start + RunsOut[found - 1].Count;
    *RunsUsedOut = found;
    return ErrOk;
}

int FreeBlockRange(uint64_t Start, uint64_t Count)
{
    if (Count == 0)
        return ErrIo;

    Superblock* super = nullptr;
    int status = SuperForBlocks(&super);
    if (status != ErrOk)
        return status;
    // Overflow-proof range: Start + Count never wraps past TotalBlocks.
    if (Count > super->TotalBlocks - Start)
        return ErrCorrupt;

    // Phase one: every bit must read used. DataStart (root dir), anything
    // below it and the backup copy are never handed out, so naming one
    // here is corruption, not a free.
    for (uint64_t index = 0; index < Count; index++)
    {
        uint64_t bit = Start + index;
        if (bit <= super->DataStart || bit == super->BackupBlock)
            return ErrCorrupt;

        bool used = false;
        status = ReadBit(super->BlockBitmapStart, super->BlockBitmapBlocks,
                         bit, &used);
        if (status != ErrOk)
            return status;
        if (!used)
            return ErrCorrupt; // double free: already 0 on disk
    }

    // Phase two: clear the bits, then credit the counter once per block.
    for (uint64_t index = 0; index < Count; index++)
    {
        status = FlipBit(super->BlockBitmapStart, super->BlockBitmapBlocks,
                         Start + index, false);
        if (status != ErrOk)
            return status;

        if (super->FreeBlocks >= super->TotalBlocks)
            return ErrCorrupt;
        super->FreeBlocks++;
    }

    return ErrOk;
}

int AllocInode(uint64_t* InodeOut)
{
    if (InodeOut == nullptr)
        return ErrIo;

    Superblock* super = nullptr;
    int status = SuperForInodes(&super);
    if (status != ErrOk)
        return status;
    if (super->FreeInodes == 0)
        return ErrNoMem;

    // First-fit from the hint, wrapping past the reserved pair (0 invalid,
    // 1 root) instead of to inode 0.
    uint64_t span = super->TotalInodes - 2;
    uint64_t cursor = InodeHintGet(super);
    for (uint64_t step = 0; step < span; step++)
    {
        uint64_t inode = 2 + (cursor - 2 + step) % span;

        bool used = true;
        status = ReadBit(super->InodeBitmapStart, super->InodeBitmapBlocks,
                         inode, &used);
        if (status != ErrOk)
            return status;
        if (used)
            continue;

        status = FlipBit(super->InodeBitmapStart, super->InodeBitmapBlocks,
                         inode, true);
        if (status != ErrOk)
            return status;

        super->FreeInodes--;
        InodeHint = inode + 1;
        *InodeOut = inode;
        return ErrOk;
    }

    return ErrNoMem;
}

int FreeInode(uint64_t Inode)
{
    Superblock* super = nullptr;
    int status = SuperForInodes(&super);
    if (status != ErrOk)
        return status;
    if (Inode < 2 || Inode >= super->TotalInodes)
        return ErrCorrupt; // 0 invalid, 1 root: neither is ever freed

    bool used = false;
    status = ReadBit(super->InodeBitmapStart, super->InodeBitmapBlocks,
                     Inode, &used);
    if (status != ErrOk)
        return status;
    if (!used)
        return ErrCorrupt; // double free

    status = FlipBit(super->InodeBitmapStart, super->InodeBitmapBlocks,
                     Inode, false);
    if (status != ErrOk)
        return status;

    if (super->FreeInodes >= super->TotalInodes)
        return ErrCorrupt;
    super->FreeInodes++;
    return ErrOk;
}

int IsBlockUsed(uint64_t Block, bool* UsedOut)
{
    if (UsedOut == nullptr)
        return ErrIo;

    Superblock* super = nullptr;
    int status = SuperForBlocks(&super);
    if (status != ErrOk)
        return status;
    if (Block >= super->TotalBlocks)
        return ErrCorrupt;

    return ReadBit(super->BlockBitmapStart, super->BlockBitmapBlocks,
                   Block, UsedOut);
}

} // namespace Fs2


