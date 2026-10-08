/* SPDX-License-Identifier: BSD-3-Clause */
// On-disk layout of 2fs v1 (docs/2fs.md): packed structs plus the geometry
// and flag constants they need, gathered in namespace Fs2. All integers are
// little endian and block numbers are volume-relative (block size 4096, the
// volume starts at Fs2::StartLba on disk). This header is C++ only; the
// C-callable filesystem API lives in include/fs2.h.
#ifndef KERNEL_FS_2FS_2FSLAYOUT_H
#define KERNEL_FS_2FS_2FSLAYOUT_H

#include <lib/stdint.h>
#include <errors.h>
#include <kernel/drivers/block.h>

namespace Fs2 {

// --- Geometry -------------------------------------------------------------
constexpr uint64_t StartLba           = 2048;   // Fs2StartLba in docs/2fs.md
constexpr uint64_t SectorSize         = 512;
constexpr uint64_t SectorsPerBlock    = 8;      // LBA = StartLba + Block * 8
constexpr uint64_t BlockSize          = 4096;
constexpr uint64_t SuperblockBlock    = 0;      // block 0: superblock
constexpr uint64_t ReservedBlock      = 1;      // block 1: reserved, zero
constexpr uint64_t BitsPerBitmapBlock = 32768;  // one bit per block or inode
constexpr uint64_t InodesPerBlock     = 16;     // 4096 / 256; TotalInodes % 16
constexpr uint64_t RootInode          = 1;      // inode 0 is invalid

// --- Superblock -----------------------------------------------------------
constexpr uint64_t Magic        = 0x4E4552572D534632ULL; // bytes "2FS-WREN"
constexpr uint16_t VersionMajor = 1;
constexpr uint16_t VersionMinor = 0;
constexpr uint32_t SuperFlagDirty = 1u << 0;    // set while mounted
constexpr uint32_t SuperFlagError = 1u << 1;

// --- Inode ----------------------------------------------------------------
constexpr uint32_t InodeSize     = 256;
constexpr uint16_t ModeTypeShift = 12;          // bits 12-15: file type
constexpr uint16_t ModeTypeMask  = 0xF000;
constexpr uint16_t ModePermMask  = 0x0FFF;      // bits 0-11: permissions
constexpr uint16_t TypeFile      = 1;
constexpr uint16_t TypeDir       = 2;
constexpr uint16_t TypeSymlink   = 3;
constexpr uint16_t InodeFlagExec       = 1u << 0;
constexpr uint16_t InodeFlagSystem     = 1u << 1;
constexpr uint16_t InodeFlagHidden     = 1u << 2;
constexpr uint16_t InodeFlagImmutable  = 1u << 3;
constexpr uint16_t InodeFlagContiguous = 1u << 4;

// --- Extents --------------------------------------------------------------
constexpr uint32_t InlineExtents   = 6;         // per inode (144 bytes)
constexpr uint32_t IndirectExtents = 170;       // per IndirectBlock
constexpr uint32_t MaxExtents      = 176;       // inline + indirect

// --- Directory entries ----------------------------------------------------
constexpr uint16_t RecLenAlign = 8;             // RecLen is a multiple of 8
constexpr uint32_t NameMax     = 255;           // bytes, no '/' and no NUL

// 4096-byte superblock, always in block 0, rest of the block zero.
struct __attribute__((packed)) Superblock {
    uint64_t Magic;             // Magic
    uint16_t VersionMajor;      // VersionMajor
    uint16_t VersionMinor;      // VersionMinor
    uint32_t BlockSize;         // BlockSize (4096)
    uint64_t TotalBlocks;
    uint64_t FreeBlocks;
    uint64_t TotalInodes;
    uint64_t FreeInodes;
    uint32_t InodeSize;         // InodeSize (256)
    uint32_t Flags;             // SuperFlag* bits
    uint64_t BlockBitmapStart;  // block numbers, volume-relative
    uint64_t BlockBitmapBlocks;
    uint64_t InodeBitmapStart;
    uint64_t InodeBitmapBlocks;
    uint64_t InodeTableStart;
    uint64_t InodeTableBlocks;
    uint64_t DataStart;         // first data block
    uint64_t RootInode;         // RootInode (1)
    uint64_t BackupBlock;       // last block of the volume
    uint64_t BootFileBlock;     // first block of the boot file, 0 = none
    uint64_t BootFileBlocks;    // contiguous with BootFileBlock
    uint64_t BootFileSize;      // bytes
    uint8_t  Uuid[16];
    char     Label[64];         // UTF-8, NUL terminated
    uint64_t CreatedTime;       // seconds since 1970
    uint64_t MountedTime;
    uint64_t WrittenTime;
    uint32_t MountCount;
    uint8_t  Reserved[3832];    // 260..4092, zero
    uint32_t Checksum;          // CRC32 of bytes 0..4091
};

// 24-byte extent: one contiguous run of blocks.
struct __attribute__((packed)) Extent {
    uint64_t FileBlock;         // first logical block in the file
    uint64_t DiskBlock;         // first block in the volume
    uint32_t Count;             // blocks
    uint32_t Flags;             // 0
};

// 256-byte inode. The first 6 extents live inline, the rest in the block
// named by IndirectBlock (up to 170 more extents).
struct __attribute__((packed)) Inode {
    uint16_t Mode;              // ModeType* in bits 12-15, perms in 0-11
    uint16_t Flags;             // InodeFlag* bits
    uint32_t Uid;
    uint32_t Gid;
    uint32_t Links;
    uint64_t Size;              // bytes
    uint64_t Blocks;            // allocated blocks (data + indirect)
    uint64_t Atime;
    uint64_t Mtime;
    uint64_t Ctime;
    uint64_t Crtime;
    uint32_t ExtentCount;       // inline + indirect
    uint32_t Reserved;
    uint64_t IndirectBlock;     // block with 170 more extents, 0 = none
    char     ExtTag[8];         // lowercase extension, NUL padded, "" for dirs
    uint64_t InodeNo;
    Extent   Extents[6];        // inline extents (144 bytes)
    uint32_t Checksum;          // CRC32 of the 256 bytes with this field as 0
    uint8_t  ReservedTail[12];  // 244..256, zero
};

// 16-byte directory-entry header; Name follows inline for NameLen bytes
// (no NUL) and the record's RecLen covers the whole entry, so entries never
// cross a block boundary. InodeNo 0 marks a free slot.
struct __attribute__((packed)) DirEntryHeader {
    uint64_t InodeNo;
    uint16_t RecLen;            // multiple of 8, reaches the block end last
    uint8_t  NameLen;           // 1..255
    uint8_t  FileType;          // TypeFile, TypeDir or TypeSymlink
    uint32_t NameHash;          // FNV-1a 32 of the name
    char     Name[];            // NameLen bytes, no NUL
};

static_assert(sizeof(Superblock) == 4096, "Fs2::Superblock must be 4096 bytes");
static_assert(sizeof(Inode) == 256, "Fs2::Inode must be 256 bytes");
static_assert(sizeof(Extent) == 24, "Fs2::Extent must be 24 bytes");
static_assert(sizeof(DirEntryHeader) == 16, "Fs2::DirEntryHeader must be 16 bytes");

// --- Block cache (2fsCache.cpp) --------------------------------------------
// Static pool of CacheBlockCount buffers of BlockSize bytes each, 16-byte
// aligned, read and written through the bound BlockDevice as
// LBA = StartLba + Block * 8 (one block is 8 sectors of 512 bytes). GetBlock
// and NewBlock pin the slot (Use) and advance the LRU stamp; release the pin
// with PutBlock. Replacement picks the least recently used unpinned slot, and
// a dirty victim is written back before its slot is reused. All entry points
// return ErrOk or a negative errors.h code and never allocate dynamically. CacheBind refuses to
// run over dirty blocks (FlushAll first); CacheUnbind flushes, drops the
// blocks and unbinds, and fails when the flush does.
constexpr uint32_t CacheBlockCount = 64;
constexpr uint32_t CacheAlignment  = 16;

int CacheBind(BlockDevice* Device, uint64_t StartLba); // bind, drop old blocks
int CacheUnbind(void);                                 // flush, drop, unbind
int GetBlock(uint64_t Block, uint8_t** BufferOut);     // read through, pin
int MarkDirty(uint64_t Block);                         // resident blocks only
int PutBlock(uint64_t Block);                          // unpin (ErrIo if not)
int FlushBlock(uint64_t Block);                        // write back if dirty
int FlushAll(void);                                    // write back + Flush
int ZeroBlock(uint64_t Block);                         // zero resident, no I/O
int NewBlock(uint64_t Block, uint8_t** BufferOut);     // zeroed slot, no read
Superblock* SuperGet(void);                            // live superblock, if any

// --- Bitmap allocators (2fsBitmap.cpp) --------------------------------------
// First-fit over the on-disk bitmaps (bit N = block/inode N, 1 = used),
// read and written through the cache above. Blocks DataStart and below plus
// the backup copy are never handed out; inode 0 is invalid and 1 is the
// root, so AllocInode never returns either. AllocBlocks reports the winning
// run(s) in RunsOut: contiguous mode fills exactly one run, scattered mode
// packs as many runs as fit (RunsCapacity) and fails with ErrNoMem when the
// free blocks - or room to report them - do not cover Count. Nothing is
// committed on a failed call. FreeBlockRange and FreeInode verify the whole
// range first (used bits only), then clear: a double free, a range crossing
// the backup copy or the volume end, or a counter that disagrees with the
// bits reads as ErrCorrupt. Placement hints (last position) make repeated
// allocations resume the scan instead of restarting it. No dynamic memory;
// every entry point returns an errors.h code.
struct AllocRun {
    uint64_t Start; // first block of the run
    uint64_t Count; // blocks in the run
};

int AllocBlock(uint64_t* BlockOut);                    // one block, any spot
int AllocBlocks(uint64_t Count, bool Contiguous, AllocRun* RunsOut,
                uint32_t RunsCapacity, uint32_t* RunsUsedOut);
int FreeBlockRange(uint64_t Start, uint64_t Count);    // all used, then clear
int AllocInode(uint64_t* InodeOut);                    // never 0 or 1
int FreeInode(uint64_t Inode);                         // never 0 or 1
int IsBlockUsed(uint64_t Block, bool* UsedOut);        // raw bitmap read

} // namespace Fs2

#endif

