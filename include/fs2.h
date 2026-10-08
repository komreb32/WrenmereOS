/* SPDX-License-Identifier: BSD-3-Clause */
// Public API of the 2fs v1 filesystem (docs/2fs.md). One volume can be
// mounted at a time: Fs2Mount binds a BlockDevice plus the volume's start
// LBA, and every later call works on that mount by path. All functions return
// ErrOk (0) or a negative code from errors.h. Freestanding: no libc and no
// dynamic memory behind this interface. The header is valid C and C++
// (extern "C" below). Note that Fs2Stat is a struct tag without a typedef -
// a function shares the plain name, so callers declare `struct Fs2Stat`,
// exactly like POSIX `stat` in C.
#ifndef FS2_H
#define FS2_H

#include <lib/stdint.h>
#include <errors.h>
#include <kernel/drivers/block.h>

// First LBA of the volume on disk (docs/2fs.md); Fs2::StartLba in C++.
#define Fs2StartLba 2048u

#ifdef __cplusplus
extern "C" {
#endif

// Metadata of one inode as Fs2Stat reports it (mirror of the on-disk inode).
struct Fs2Stat {
    uint64_t Inode;     // inode number
    uint16_t Mode;      // bits 12-15 type (1 file, 2 dir, 3 symlink), perms in 0-11
    uint16_t Flags;     // bit0 Exec, bit1 System, bit2 Hidden, bit3 Immutable,
                        // bit4 Contiguous
    uint32_t Uid;
    uint32_t Gid;
    uint32_t Links;     // hard links
    uint64_t Size;      // bytes
    uint64_t Blocks;    // allocated blocks (data + indirect)
    uint64_t Atime;     // seconds since 1970
    uint64_t Mtime;
    uint64_t Ctime;
    uint64_t Crtime;
    char     ExtTag[8]; // lowercase extension without dot, NUL padded ("" for dirs)
};

// One directory entry as Fs2ReadDir hands it out: the variable-length
// on-disk record flattened into a fixed slot with a NUL-terminated name.
typedef struct Fs2DirEntry {
    uint64_t Inode;     // inode number of the entry
    uint8_t  FileType;  // 1 file, 2 dir, 3 symlink
    char     Name[256]; // UTF-8, NUL terminated, 1..255 bytes
} Fs2DirEntry;

// Bind Device as the mounted volume whose first LBA is StartLba (use
// Fs2StartLba for the built-in volume): reads and verifies the superblock
// (magic, version, checksum) and marks it dirty. Returns ErrIo when the
// device refuses, the superblock is invalid or a volume is already mounted.
int Fs2Mount(BlockDevice* Device, uint64_t StartLba);

// Flush the superblock, clear the dirty flag and release the mount.
int Fs2Unmount(void);

// Flush every cached write through the device (superblock included).
int Fs2Sync(void);

// Report the volume label (NUL-terminated, LabelOut holds at least 64
// bytes) and the block counts from the superblock.
int Fs2Info(char* LabelOut, uint64_t* TotalBlocks, uint64_t* FreeBlocks);

// Fill Out with the metadata of the inode at Path.
int Fs2Stat(const char* Path, struct Fs2Stat* Out);

// Fill Out with directory entry Index (0-based) of the directory at Path;
// entries are returned in on-disk order, including "." and "..".
int Fs2ReadDir(const char* Path, uint32_t Index, Fs2DirEntry* Out);

// Create an empty regular file at Path with the given Mode (permissions).
int Fs2Create(const char* Path, uint16_t Mode);

// Create an empty directory at Path with the given Mode; "." and ".." are
// created inside it.
int Fs2Mkdir(const char* Path, uint16_t Mode);

// Remove the non-directory file at Path; the inode is freed once its last
// link and open handles are gone.
int Fs2Unlink(const char* Path);

// Remove the empty directory at Path (fails with ErrIo while non-empty).
int Fs2Rmdir(const char* Path);

// Rename From to To, moving the inode; To must not exist.
int Fs2Rename(const char* From, const char* To);

// Read Length bytes of the file at Path starting at Offset into Buffer;
// ReadOut receives the bytes actually read (short at end of file).
int Fs2Read(const char* Path, uint64_t Offset, void* Buffer, uint64_t Length, uint64_t* ReadOut);

// Write Length bytes of Buffer into the file at Path starting at Offset,
// growing it as needed; WrittenOut receives the bytes actually written.
int Fs2Write(const char* Path, uint64_t Offset, const void* Buffer, uint64_t Length, uint64_t* WrittenOut);

// Grow or shrink the file at Path to Size bytes; holes read back as zero.
int Fs2Truncate(const char* Path, uint64_t Size);

// Mark the file at Path as the boot file: records its first block, block
// count and size in the superblock (must be contiguous on disk).
int Fs2SetBoot(const char* Path);

// Seconds since 1970 used to stamp inode times. The weak default returns 0
// until the RTC driver links a strong definition; that implementation should
// define FS2_NO_DEFAULT_NOW before including this header (or not include it
// at all) so both definitions never share one translation unit.
uint64_t Fs2Now(void);

#ifndef FS2_NO_DEFAULT_NOW
__attribute__((weak)) uint64_t Fs2Now(void) { return 0; }
#endif

#ifdef __cplusplus
}
#endif

#endif
