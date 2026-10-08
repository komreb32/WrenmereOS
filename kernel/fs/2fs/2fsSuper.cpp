/* SPDX-License-Identifier: BSD-3-Clause */
// 2fs superblock management: mount, unmount, sync and volume info over an
// in-memory copy of the on-disk superblock. Fs2Mount reads block 0 and
// validates it (magic, version, sizes, checksum and region ranges); when the
// primary fails it tries the copy at BackupBlock and restores it. Mount then
// marks the volume dirty, bumps MountCount and stamps MountedTime; unmount
// flushes the cache, clears the flag and writes the superblock plus its
// copy. Every entry point returns an errors.h code and nothing is allocated
// dynamically.
#include "2fsLayout.h"             // Fs2::Superblock, constants, block cache
#include "../../archive/exf/exf.h" // Crc32, shared with the EXF checksum
#include <fs2.h>                   // Fs2Mount & friends, Fs2Now

// lib/stdio.h carries no extern "C" guard of its own and the console is C.
extern "C" {
#include <lib/stdio.h>
}

namespace {

// Global volume state: the mounted device, where the volume starts on disk
// and the superblock as known in memory (persisted by SuperStore).
struct VolumeState {
    BlockDevice*    Device;   // non-NULL exactly while mounted
    uint64_t        StartLba;
    Fs2::Superblock Super;
};

VolumeState Volume;

// Freestanding byte copy: the kernel has no libc memcpy.
void MemCopy(void* Destination, const void* Source, uint64_t Length)
{
    uint8_t* destination = (uint8_t*)Destination;
    const uint8_t* source = (const uint8_t*)Source;

    for (uint64_t index = 0; index < Length; index++)
        destination[index] = source[index];
}

// True when [Start, Start + Blocks) lies inside the volume; written so the
// addition itself can never overflow.
bool RegionFits(uint64_t Start, uint64_t Blocks, uint64_t TotalBlocks)
{
    if (Blocks == 0 || Start >= TotalBlocks)
        return false;

    return Blocks <= TotalBlocks - Start;
}

// Full validation of a superblock image: identity fields, sizes, the CRC32
// over bytes 0..4091 (the Checksum field itself reads as zero, see
// docs/2fs.md) and the layout ranges - contiguous bitmap, table and data
// regions after the reserved blocks, backup copy inside the data area, as
// tools/2fs.py lays the volume out.
bool SuperValid(const Fs2::Superblock* Sb)
{
    if (Sb->Magic != Fs2::Magic)
        return false;
    if (Sb->VersionMajor != Fs2::VersionMajor || Sb->VersionMinor != Fs2::VersionMinor)
        return false;
    if (Sb->BlockSize != Fs2::BlockSize)
        return false;
    if (Sb->InodeSize != Fs2::InodeSize)
        return false;
    if (Sb->TotalBlocks == 0)
        return false;
    if (Crc32(Sb, __builtin_offsetof(Fs2::Superblock, Checksum)) != Sb->Checksum)
        return false;

    if (Sb->BlockBitmapStart < 2) // block 0 superblock, block 1 reserved
        return false;
    if (!RegionFits(Sb->BlockBitmapStart, Sb->BlockBitmapBlocks, Sb->TotalBlocks))
        return false;
    if (Sb->InodeBitmapStart != Sb->BlockBitmapStart + Sb->BlockBitmapBlocks)
        return false;
    if (!RegionFits(Sb->InodeBitmapStart, Sb->InodeBitmapBlocks, Sb->TotalBlocks))
        return false;
    if (Sb->InodeTableStart != Sb->InodeBitmapStart + Sb->InodeBitmapBlocks)
        return false;
    if (!RegionFits(Sb->InodeTableStart, Sb->InodeTableBlocks, Sb->TotalBlocks))
        return false;
    if (Sb->DataStart != Sb->InodeTableStart + Sb->InodeTableBlocks)
        return false;
    if (Sb->DataStart >= Sb->TotalBlocks)
        return false;
    if (Sb->BackupBlock <= Sb->DataStart) // mkfs keeps root dir + backup apart
        return false;
    if (Sb->BackupBlock >= Sb->TotalBlocks)
        return false;

    return true;
}

// Write one block of the volume through the cache, immediately flushed, so
// SuperStore's result is on disk when it returns.
int StoreBlock(uint64_t Block, const Fs2::Superblock* Source)
{
    uint8_t* buffer = nullptr;
    int status = Fs2::GetBlock(Block, &buffer);
    if (status != ErrOk)
        return status;

    MemCopy(buffer, Source, sizeof(Fs2::Superblock));
    status = Fs2::MarkDirty(Block);
    if (status == ErrOk)
        status = Fs2::FlushBlock(Block);

    int released = Fs2::PutBlock(Block);
    return status != ErrOk ? status : released;
}

// Recompute the checksum over bytes 0..4091 and write the superblock to
// block 0 plus its copy to BackupBlock.
int SuperStore(void)
{
    Volume.Super.Checksum = 0;
    Volume.Super.WrittenTime = Fs2Now();
    Volume.Super.Checksum =
        Crc32(&Volume.Super, __builtin_offsetof(Fs2::Superblock, Checksum));

    int status = StoreBlock(Fs2::SuperblockBlock, &Volume.Super);
    if (status != ErrOk)
        return status;

    return StoreBlock(Volume.Super.BackupBlock, &Volume.Super);
}

// Load the superblock into the global state. The primary (block 0) wins when
// it validates; otherwise the copy is read from BackupBlock - falling back to
// the last block of the volume when the primary's own fields disagree - and,
// once validated, written back as the primary.
int SuperLoad(void)
{
    uint8_t* buffer = nullptr;

    int status = Fs2::GetBlock(Fs2::SuperblockBlock, &buffer);
    if (status != ErrOk)
        return status;

    MemCopy(&Volume.Super, buffer, sizeof(Fs2::Superblock));
    status = Fs2::PutBlock(Fs2::SuperblockBlock);
    if (status != ErrOk)
        return status;

    if (SuperValid(&Volume.Super))
        return ErrOk;

    uint64_t candidates[2] = { 0, 0 };
    uint32_t count = 0;
    if (Volume.Super.BackupBlock != 0)
        candidates[count++] = Volume.Super.BackupBlock;
    uint64_t last = Volume.Super.TotalBlocks > 1 ? Volume.Super.TotalBlocks - 1 : 0;
    if (last != 0 && last != candidates[0])
        candidates[count++] = last;

    for (uint32_t index = 0; index < count; index++)
    {
        if (candidates[index] == Fs2::SuperblockBlock)
            continue;

        status = Fs2::GetBlock(candidates[index], &buffer);
        if (status != ErrOk)
            continue; // unreadable candidate: try the next one

        bool valid = SuperValid(reinterpret_cast<const Fs2::Superblock*>(buffer));
        if (valid)
            MemCopy(&Volume.Super, buffer, sizeof(Fs2::Superblock));

        int released = Fs2::PutBlock(candidates[index]);
        if (!valid)
            continue;
        if (released != ErrOk)
            return released;

        return SuperStore(); // restore the copy as the primary
    }

    return ErrIo;
}

} // namespace

namespace Fs2 {

// Live in-memory superblock for the bitmap allocator (declared in
// 2fsLayout.h): NULL while unmounted so callers cannot move counters
// without a volume behind them.
Superblock* SuperGet(void)
{
    if (Volume.Device == nullptr)
        return nullptr;

    return &Volume.Super;
}

} // namespace Fs2

int Fs2Mount(BlockDevice* Device, uint64_t StartLba)
{
    if (Device == nullptr || Device->Read == nullptr ||
        Device->Write == nullptr || Device->Flush == nullptr)
        return ErrIo;
    if (Volume.Device != nullptr)
        return ErrIo; // already mounted

    int status = Fs2::CacheBind(Device, StartLba);
    if (status != ErrOk)
        return status;

    status = SuperLoad();
    if (status != ErrOk)
    {
        Fs2::CacheUnbind(); // drop the binding, retrying any pending write
        return status;
    }

    if ((Volume.Super.Flags & Fs2::SuperFlagDirty) != 0)
        KInfo("2fs: volume is dirty, the previous unmount did not finish");

    Volume.Super.Flags |= Fs2::SuperFlagDirty;
    Volume.Super.MountCount++;
    Volume.Super.MountedTime = Fs2Now();

    status = SuperStore();
    if (status != ErrOk)
    {
        Fs2::CacheUnbind();
        return status;
    }

    Volume.Device = Device;
    Volume.StartLba = StartLba;
    return ErrOk;
}

int Fs2Unmount(void)
{
    if (Volume.Device == nullptr)
        return ErrIo;

    int status = Fs2::FlushAll();
    if (status != ErrOk)
        return status; // stay mounted: pending data keeps the dirty flag

    Volume.Super.Flags &= ~Fs2::SuperFlagDirty;
    int stored = SuperStore();
    int unbound = Fs2::CacheUnbind();

    Volume.Device = nullptr;
    Volume.StartLba = 0;

    if (stored != ErrOk)
        return stored;
    return unbound;
}

int Fs2Sync(void)
{
    if (Volume.Device == nullptr)
        return ErrIo;

    int status = Fs2::FlushAll();
    if (status != ErrOk)
        return status;

    return SuperStore();
}

int Fs2Info(char* LabelOut, uint64_t* TotalBlocks, uint64_t* FreeBlocks)
{
    if (Volume.Device == nullptr)
        return ErrIo;

    if (LabelOut != nullptr)
    {
        uint64_t length = 0;
        while (length + 1 < sizeof(Volume.Super.Label) &&
               Volume.Super.Label[length] != '\0')
        {
            LabelOut[length] = Volume.Super.Label[length];
            length++;
        }
        LabelOut[length] = '\0'; // always NUL-terminated inside 64 bytes
    }

    if (TotalBlocks != nullptr)
        *TotalBlocks = Volume.Super.TotalBlocks;
    if (FreeBlocks != nullptr)
        *FreeBlocks = Volume.Super.FreeBlocks;

    return ErrOk;
}
