/* SPDX-License-Identifier: BSD-3-Clause */
// 2fs inode management (docs/2fs.md): InodeRead/InodeWrite over the inode
// table with CRC32 verification, InodeCreate over the bitmap allocator,
// MapFileBlock across the 6 inline extents plus the 170-entry indirect
// table, AppendExtent with tail merging and indirect spillover,
// GrowInode/ShrinkInode by whole blocks and FreeInodeData. An Inode struct
// here is a caller-owned in-memory copy: mutators change the copy and only
// touch the disk when they persist it through InodeWrite, so a failed call
// leaves the on-disk inode exactly as it was. No dynamic memory is used
// and every entry point returns an errors.h code.
#include "2fsLayout.h"
#include "../../archive/exf/exf.h" // Crc32, shared with the superblock checksum
#include <fs2.h>                   // Fs2Now for the inode timestamps

namespace Fs2 {
namespace {

// Inode table slot for InodeNo: one of 16 packed records in a cached table
// block. The caller keeps the block pinned while it copies the record.
int InodeSlot(uint64_t InodeNo, uint64_t* BlockOut, uint64_t* OffsetOut)
{
    const Superblock* super = SuperGet();
    if (super == nullptr)
        return ErrIo; // no mounted volume behind the inode table

    if (InodeNo == 0 || InodeNo >= super->TotalInodes)
        return ErrCorrupt;
    if (super->InodeTableBlocks == 0)
        return ErrCorrupt;

    uint64_t index = InodeNo / InodesPerBlock;
    if (index >= super->InodeTableBlocks)
        return ErrCorrupt;

    *BlockOut = super->InodeTableStart + index;
    *OffsetOut = (InodeNo % InodesPerBlock) * InodeSize;
    return ErrOk;
}

// CRC32 of the 256-byte record with the Checksum field read as zero, the
// same wire form tools/2fs.py packs and verifies.
uint32_t InodeCrc(const Inode* Record)
{
    Inode probe = *Record;
    probe.Checksum = 0;
    return Crc32(&probe, sizeof(Inode));
}

} // namespace

int InodeRead(uint64_t InodeNo, Inode* Out)
{
    if (Out == nullptr)
        return ErrIo;

    uint64_t block = 0;
    uint64_t offset = 0;
    int status = InodeSlot(InodeNo, &block, &offset);
    if (status != ErrOk)
        return status;

    uint8_t* buffer = nullptr;
    status = GetBlock(block, &buffer);
    if (status != ErrOk)
        return status;

    Inode record = *reinterpret_cast<const Inode*>(buffer + offset);
    status = PutBlock(block);
    if (status != ErrOk)
        return status;

    if (record.InodeNo != InodeNo)
        return ErrCorrupt;
    if (InodeCrc(&record) != record.Checksum)
        return ErrCorrupt;

    *Out = record;
    return ErrOk;
}

int InodeWrite(uint64_t InodeNo, const Inode* Record)
{
    if (Record == nullptr)
        return ErrIo;

    uint64_t block = 0;
    uint64_t offset = 0;
    int status = InodeSlot(InodeNo, &block, &offset);
    if (status != ErrOk)
        return status;

    Inode copy = *Record;
    copy.InodeNo = InodeNo;
    copy.Checksum = InodeCrc(&copy);

    uint8_t* buffer = nullptr;
    status = GetBlock(block, &buffer);
    if (status != ErrOk)
        return status;

    *reinterpret_cast<Inode*>(buffer + offset) = copy;
    status = MarkDirty(block);
    int released = PutBlock(block);
    if (status != ErrOk)
        return status;
    return released;
}

int InodeCreate(uint16_t Mode, uint16_t Flags, const char* ExtTag,
                uint64_t* InodeOut)
{
    if (InodeOut == nullptr)
        return ErrIo;

    uint16_t type = (uint16_t)((Mode & ModeTypeMask) >> ModeTypeShift);
    if (type == 0)
        type = TypeFile;
    if (type != TypeFile && type != TypeDir && type != TypeSymlink)
        return ErrIo;

    uint64_t inodeNo = 0;
    int status = AllocInode(&inodeNo);
    if (status != ErrOk)
        return status;

    // Normalize the tag: lowercase ASCII, NUL padded, cut at 8 bytes, the
    // same form tools/2fs.py packs and Fs2Stat reports.
    char tag[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (type != TypeDir && ExtTag != nullptr)
    {
        for (uint32_t index = 0; index < 8 && ExtTag[index] != '\0'; index++)
        {
            char letter = ExtTag[index];
            if (letter >= 'A' && letter <= 'Z')
                letter = (char)(letter + ('a' - 'A'));
            tag[index] = letter;
        }
    }

    Inode record = {};
    record.Mode = (uint16_t)((Mode & ModePermMask) |
                             (type << ModeTypeShift));
    record.Flags = Flags;
    record.Uid = 0;
    record.Gid = 0;
    record.Links = 1;
    record.Size = 0;
    record.Blocks = 0;
    record.Atime = Fs2Now();
    record.Mtime = record.Atime;
    record.Ctime = record.Atime;
    record.Crtime = record.Atime;
    record.ExtentCount = 0;
    record.Reserved = 0;
    record.IndirectBlock = 0;
    for (uint32_t index = 0; index < 8; index++)
        record.ExtTag[index] = tag[index];
    record.InodeNo = inodeNo;
    for (uint32_t index = 0; index < InlineExtents; index++)
    {
        record.Extents[index].FileBlock = 0;
        record.Extents[index].DiskBlock = 0;
        record.Extents[index].Count = 0;
        record.Extents[index].Flags = 0;
    }
    for (uint32_t index = 0; index < 12; index++)
        record.ReservedTail[index] = 0;
    record.Checksum = 0;

    status = InodeWrite(inodeNo, &record);
    if (status != ErrOk)
    {
        FreeInode(inodeNo); // the table slot is garbage: give the bit back
        return status;
    }

    *InodeOut = inodeNo;
    return ErrOk;
}

int MapFileBlock(const Inode* Record, uint64_t FileBlock, uint64_t* DiskOut)
{
    if (Record == nullptr || DiskOut == nullptr)
        return ErrIo;

    for (uint32_t index = 0; index < InlineExtents; index++)
    {
        const Extent* extent = &Record->Extents[index];
        if (extent->Count == 0)
            continue;
        if (extent->FileBlock <= FileBlock &&
            FileBlock < extent->FileBlock + extent->Count)
        {
            *DiskOut = extent->DiskBlock + (FileBlock - extent->FileBlock);
            return ErrOk;
        }
    }

    if (Record->IndirectBlock == 0)
        return ErrNoMem; // a hole: no disk block behind FileBlock

    uint8_t* buffer = nullptr;
    int status = GetBlock(Record->IndirectBlock, &buffer);
    if (status != ErrOk)
        return status;

    const Extent* table = reinterpret_cast<const Extent*>(buffer);
    int found = ErrNoMem;
    for (uint32_t index = 0; index < IndirectExtents; index++)
    {
        if (table[index].Count == 0)
            continue;
        if (table[index].FileBlock <= FileBlock &&
            FileBlock < table[index].FileBlock + table[index].Count)
        {
            *DiskOut = table[index].DiskBlock + (FileBlock - table[index].FileBlock);
            found = ErrOk;
            break;
        }
    }

    int released = PutBlock(Record->IndirectBlock);
    if (found != ErrOk)
        return found;
    return released;
}

int AppendExtent(Inode* Record, uint64_t DiskBlock, uint32_t Count, uint64_t FileBlock)
{
    if (Record == nullptr || Count == 0)
        return ErrIo;

    if (Record->IndirectBlock != 0)
    {
        uint8_t* buffer = nullptr;
        int status = GetBlock(Record->IndirectBlock, &buffer);
        if (status != ErrOk)
            return status;

        Extent* indirect = reinterpret_cast<Extent*>(buffer);
        int lastIndirect = -1;
        int firstFree = -1;
        for (uint32_t index = 0; index < IndirectExtents; index++)
        {
            if (indirect[index].Count != 0)
            {
                lastIndirect = (int)index;
            }
            else if (firstFree < 0)
            {
                firstFree = (int)index;
            }
        }

        if (lastIndirect >= 0)
        {
            Extent* last = &indirect[lastIndirect];
            if (last->FileBlock + last->Count == FileBlock &&
                last->DiskBlock + last->Count == DiskBlock)
            {
                last->Count += Count;
                status = MarkDirty(Record->IndirectBlock);
                int released = PutBlock(Record->IndirectBlock);
                if (status != ErrOk)
                    return status;
                return released;
            }
        }

        if (firstFree < 0)
        {
            PutBlock(Record->IndirectBlock);
            return ErrNoMem;
        }

        indirect[firstFree].FileBlock = FileBlock;
        indirect[firstFree].DiskBlock = DiskBlock;
        indirect[firstFree].Count = Count;
        indirect[firstFree].Flags = 0;
        Record->ExtentCount++;

        status = MarkDirty(Record->IndirectBlock);
        int released = PutBlock(Record->IndirectBlock);
        if (status != ErrOk)
            return status;
        return released;
    }

    int lastInline = -1;
    int firstFreeInline = -1;
    for (uint32_t index = 0; index < InlineExtents; index++)
    {
        if (Record->Extents[index].Count != 0)
        {
            lastInline = (int)index;
        }
        else if (firstFreeInline < 0)
        {
            firstFreeInline = (int)index;
        }
    }

    if (lastInline >= 0)
    {
        Extent* last = &Record->Extents[lastInline];
        if (last->FileBlock + last->Count == FileBlock &&
            last->DiskBlock + last->Count == DiskBlock)
        {
            last->Count += Count;
            return ErrOk;
        }
    }

    if (firstFreeInline >= 0)
    {
        Record->Extents[firstFreeInline].FileBlock = FileBlock;
        Record->Extents[firstFreeInline].DiskBlock = DiskBlock;
        Record->Extents[firstFreeInline].Count = Count;
        Record->Extents[firstFreeInline].Flags = 0;
        Record->ExtentCount++;
        return ErrOk;
    }

    // All 6 inline extents full: allocate indirect block
    uint64_t indirectBlock = 0;
    int status = AllocBlock(&indirectBlock);
    if (status != ErrOk)
        return status;

    uint8_t* buffer = nullptr;
    status = NewBlock(indirectBlock, &buffer);
    if (status != ErrOk)
    {
        FreeBlockRange(indirectBlock, 1);
        return status;
    }

    Extent* indirect = reinterpret_cast<Extent*>(buffer);
    indirect[0].FileBlock = FileBlock;
    indirect[0].DiskBlock = DiskBlock;
    indirect[0].Count = Count;
    indirect[0].Flags = 0;

    Record->IndirectBlock = indirectBlock;
    Record->Blocks++;
    Record->ExtentCount++;

    status = MarkDirty(indirectBlock);
    int released = PutBlock(indirectBlock);
    if (status != ErrOk)
        return status;
    return released;
}

int AppendExtent(Inode* Record, uint64_t DiskBlock, uint32_t Count)
{
    if (Record == nullptr || Count == 0)
        return ErrIo;

    uint64_t fileBlock = 0;
    if (Record->IndirectBlock != 0)
    {
        uint8_t* buffer = nullptr;
        int status = GetBlock(Record->IndirectBlock, &buffer);
        if (status != ErrOk)
            return status;

        const Extent* indirect = reinterpret_cast<const Extent*>(buffer);
        int lastIdx = -1;
        for (uint32_t index = 0; index < IndirectExtents; index++)
        {
            if (indirect[index].Count != 0)
                lastIdx = (int)index;
        }
        if (lastIdx >= 0)
            fileBlock = indirect[lastIdx].FileBlock + indirect[lastIdx].Count;
        else
            fileBlock = (Record->Size == 0) ? 0 : (Record->Size + BlockSize - 1) / BlockSize;

        PutBlock(Record->IndirectBlock);
    }
    else
    {
        int lastIdx = -1;
        for (uint32_t index = 0; index < InlineExtents; index++)
        {
            if (Record->Extents[index].Count != 0)
                lastIdx = (int)index;
        }
        if (lastIdx >= 0)
            fileBlock = Record->Extents[lastIdx].FileBlock + Record->Extents[lastIdx].Count;
        else
            fileBlock = (Record->Size == 0) ? 0 : (Record->Size + BlockSize - 1) / BlockSize;
    }

    return AppendExtent(Record, DiskBlock, Count, fileBlock);
}

int FreeInodeData(Inode* Record)
{
    if (Record == nullptr)
        return ErrIo;

    if (Record->IndirectBlock != 0)
    {
        uint8_t* buffer = nullptr;
        int status = GetBlock(Record->IndirectBlock, &buffer);
        if (status == ErrOk)
        {
            const Extent* indirect = reinterpret_cast<const Extent*>(buffer);
            for (uint32_t index = 0; index < IndirectExtents; index++)
            {
                if (indirect[index].Count != 0)
                {
                    FreeBlockRange(indirect[index].DiskBlock, indirect[index].Count);
                }
            }
            PutBlock(Record->IndirectBlock);
        }
        FreeBlockRange(Record->IndirectBlock, 1);
        Record->IndirectBlock = 0;
    }

    for (uint32_t index = 0; index < InlineExtents; index++)
    {
        if (Record->Extents[index].Count != 0)
        {
            FreeBlockRange(Record->Extents[index].DiskBlock, Record->Extents[index].Count);
            Record->Extents[index].FileBlock = 0;
            Record->Extents[index].DiskBlock = 0;
            Record->Extents[index].Count = 0;
            Record->Extents[index].Flags = 0;
        }
    }

    Record->ExtentCount = 0;
    Record->Blocks = 0;
    Record->Size = 0;
    Record->Mtime = Fs2Now();
    Record->Ctime = Record->Mtime;
    return ErrOk;
}

int ShrinkInode(Inode* Record, uint64_t NewSize)
{
    if (Record == nullptr)
        return ErrIo;
    if (NewSize > Record->Size)
        return ErrIo;
    if (NewSize == Record->Size)
        return ErrOk;
    if (NewSize == 0)
        return FreeInodeData(Record);

    uint64_t keepBlocks = (NewSize + BlockSize - 1) / BlockSize;

    if (Record->IndirectBlock != 0)
    {
        uint8_t* buffer = nullptr;
        int status = GetBlock(Record->IndirectBlock, &buffer);
        if (status != ErrOk)
            return status;

        Extent* indirect = reinterpret_cast<Extent*>(buffer);
        uint32_t remainingIndirect = 0;
        for (uint32_t index = 0; index < IndirectExtents; index++)
        {
            if (indirect[index].Count == 0)
                continue;

            if (indirect[index].FileBlock >= keepBlocks)
            {
                FreeBlockRange(indirect[index].DiskBlock, indirect[index].Count);
                Record->Blocks -= indirect[index].Count;
                indirect[index].FileBlock = 0;
                indirect[index].DiskBlock = 0;
                indirect[index].Count = 0;
                indirect[index].Flags = 0;
                Record->ExtentCount--;
            }
            else if (indirect[index].FileBlock + indirect[index].Count > keepBlocks)
            {
                uint32_t keepCount = (uint32_t)(keepBlocks - indirect[index].FileBlock);
                uint32_t freeCount = indirect[index].Count - keepCount;
                FreeBlockRange(indirect[index].DiskBlock + keepCount, freeCount);
                Record->Blocks -= freeCount;
                indirect[index].Count = keepCount;
                remainingIndirect++;
            }
            else
            {
                remainingIndirect++;
            }
        }

        if (remainingIndirect == 0)
        {
            PutBlock(Record->IndirectBlock);
            FreeBlockRange(Record->IndirectBlock, 1);
            Record->Blocks--;
            Record->IndirectBlock = 0;
        }
        else
        {
            MarkDirty(Record->IndirectBlock);
            PutBlock(Record->IndirectBlock);
        }
    }

    for (uint32_t index = 0; index < InlineExtents; index++)
    {
        Extent* ext = &Record->Extents[index];
        if (ext->Count == 0)
            continue;

        if (ext->FileBlock >= keepBlocks)
        {
            FreeBlockRange(ext->DiskBlock, ext->Count);
            Record->Blocks -= ext->Count;
            ext->FileBlock = 0;
            ext->DiskBlock = 0;
            ext->Count = 0;
            ext->Flags = 0;
            Record->ExtentCount--;
        }
        else if (ext->FileBlock + ext->Count > keepBlocks)
        {
            uint32_t keepCount = (uint32_t)(keepBlocks - ext->FileBlock);
            uint32_t freeCount = ext->Count - keepCount;
            FreeBlockRange(ext->DiskBlock + keepCount, freeCount);
            Record->Blocks -= freeCount;
            ext->Count = keepCount;
        }
    }

    Record->Size = NewSize;
    Record->Mtime = Fs2Now();
    Record->Ctime = Record->Mtime;
    return ErrOk;
}

int GrowInode(Inode* Record, uint64_t NewSize)
{
    if (Record == nullptr)
        return ErrIo;
    if (NewSize < Record->Size)
        return ErrIo;
    if (NewSize == Record->Size)
        return ErrOk;

    uint64_t oldSize = Record->Size;
    uint64_t currentBlocks = (oldSize == 0) ? 0 : (oldSize + BlockSize - 1) / BlockSize;
    uint64_t targetBlocks = (NewSize + BlockSize - 1) / BlockSize;

    if (targetBlocks > currentBlocks)
    {
        for (uint64_t fileBlock = 0; fileBlock < targetBlocks; fileBlock++)
        {
            uint64_t diskBlock = 0;
            int mapStatus = MapFileBlock(Record, fileBlock, &diskBlock);
            if (mapStatus == ErrOk)
                continue;

            int status = AllocBlock(&diskBlock);
            if (status != ErrOk)
            {
                ShrinkInode(Record, oldSize);
                return status;
            }

            uint8_t* buffer = nullptr;
            status = NewBlock(diskBlock, &buffer);
            if (status == ErrOk)
                PutBlock(diskBlock);

            status = AppendExtent(Record, diskBlock, 1, fileBlock);
            if (status != ErrOk)
            {
                FreeBlockRange(diskBlock, 1);
                ShrinkInode(Record, oldSize);
                return status;
            }

            Record->Blocks++;
        }
    }

    Record->Size = NewSize;
    Record->Mtime = Fs2Now();
    Record->Ctime = Record->Mtime;
    return ErrOk;
}

} // namespace Fs2
