/* SPDX-License-Identifier: BSD-3-Clause */
// 2fs path resolution and public C API (docs/2fs.md):
// ResolvePath splits paths by '/', traverses directories via DirLookup,
// enforces the 4096-byte path limit and 255-byte component limit, and supports
// "." and "..". ResolveParent extracts the parent directory inode and the
// basename. Implementations of include/fs2.h entry points: Fs2Stat, Fs2ReadDir,
// Fs2Create, Fs2Mkdir, Fs2Unlink, Fs2Rmdir, Fs2Rename, Fs2Read, Fs2Write,
// Fs2Truncate, and Fs2SetBoot. ExtTag is populated on file creation with the
// lowercase extension (up to 7 chars) and InodeFlagExec is set for "exf" files.
// Fs2SetBoot requires a single extent, marks InodeFlagContiguous, and updates
// BootFileBlock, BootFileBlocks, and BootFileSize in the superblock.
#include "2fsLayout.h"
#include <fs2.h>
#include <lib/string.h>

#ifndef ErrBusy
#define ErrBusy (-6)
#endif

namespace Fs2 {

// Declarations of inode mutators from 2fsInode.cpp
int InodeRead(uint64_t InodeNo, Inode* Out);
int InodeWrite(uint64_t InodeNo, const Inode* Record);
int InodeCreate(uint16_t Mode, uint16_t Flags, const char* ExtTag, uint64_t* InodeOut);

// Declarations of directory helpers from 2fsDir.cpp
int DirLookup(const Inode* DirInode, const char* Name, uint64_t* OutInode, uint8_t* OutType);
int DirLookup(uint64_t DirInodeNo, const char* Name, uint64_t* OutInode, uint8_t* OutType);
int MakeDirectory(uint64_t ParentInodeNo, const char* Name, uint16_t Mode, uint64_t* InodeOut);
int RemoveDirectory(uint64_t ParentInodeNo, const char* Name);
int DirIterate(uint64_t DirInodeNo, uint32_t Index, Fs2DirEntry* Out);
int ValidateName(const char* Name);

// Declarations of regular file helpers from 2fsFile.cpp
int FileRead(uint64_t InodeNo, uint64_t Offset, void* Buffer, uint64_t Length, uint64_t* ReadOut = nullptr);
int FileWrite(uint64_t InodeNo, uint64_t Offset, const void* Buffer, uint64_t Length, uint64_t* WrittenOut = nullptr);
int FileTruncate(uint64_t InodeNo, uint64_t NewSize);
int FileCreate(uint64_t ParentInodeNo, const char* Name, uint16_t Mode, uint64_t* InodeOut = nullptr);
int FileUnlink(uint64_t ParentInodeNo, const char* Name);
int FileRename(uint64_t OldParentInodeNo, const char* OldName, uint64_t NewParentInodeNo, const char* NewName);

// Resolve Path to an inode number and file type.
// Traverses directory components separated by '/' using DirLookup.
// Rejects paths longer than 4096 bytes or components longer than 255 bytes.
// Supports "." and "..". If MaxLen is specified, only examines up to MaxLen bytes.
int ResolvePath(const char* Path, uint64_t* InodeOut, uint8_t* TypeOut = nullptr, uint32_t MaxLen = 0xFFFFFFFF)
{
    if (Path == nullptr || InodeOut == nullptr)
        return ErrIo;

    uint32_t len = 0;
    while (len < MaxLen && Path[len] != '\0')
    {
        len++;
        if (len > 4096)
            return ErrIo;
    }

    uint64_t currentInode = RootInode;
    uint8_t currentType = TypeDir;

    // Empty path or root slash resolves to RootInode
    if (len == 0)
    {
        *InodeOut = RootInode;
        if (TypeOut != nullptr)
            *TypeOut = TypeDir;
        return ErrOk;
    }

    bool hasTrailingSlash = false;
    uint32_t end = len;
    while (end > 0 && Path[end - 1] == '/')
    {
        hasTrailingSlash = true;
        end--;
    }

    if (end == 0)
    {
        *InodeOut = RootInode;
        if (TypeOut != nullptr)
            *TypeOut = TypeDir;
        return ErrOk;
    }

    uint32_t pos = 0;
    while (pos < end)
    {
        // Skip leading or consecutive slashes
        while (pos < end && Path[pos] == '/')
            pos++;

        if (pos >= end)
            break;

        uint32_t start = pos;
        while (pos < end && Path[pos] != '/')
            pos++;

        uint32_t compLen = pos - start;
        if (compLen > NameMax)
            return ErrIo;

        char comp[256];
        for (uint32_t i = 0; i < compLen; i++)
            comp[i] = Path[start + i];
        comp[compLen] = '\0';

        uint64_t nextInode = 0;
        uint8_t nextType = 0;
        int status = DirLookup(currentInode, comp, &nextInode, &nextType);
        if (status != ErrOk)
            return status;

        currentInode = nextInode;
        currentType = nextType;
    }

    // Trailing slash requires target to be a directory
    if (hasTrailingSlash && currentType != TypeDir)
        return ErrIo;

    *InodeOut = currentInode;
    if (TypeOut != nullptr)
        *TypeOut = currentType;
    return ErrOk;
}

// Extract parent directory inode and basename component from Path.
// Returns ErrIo if Path is empty or consists only of slashes.
int ResolveParent(const char* Path, uint64_t* ParentInodeOut, char* NameOut)
{
    if (Path == nullptr || ParentInodeOut == nullptr || NameOut == nullptr)
        return ErrIo;

    uint32_t len = 0;
    while (Path[len] != '\0')
    {
        len++;
        if (len > 4096)
            return ErrIo;
    }

    if (len == 0)
        return ErrIo;

    // Strip trailing slashes
    while (len > 0 && Path[len - 1] == '/')
        len--;

    if (len == 0)
        return ErrIo; // Path was only slashes

    // Locate last component
    uint32_t compEnd = len;
    uint32_t compStart = compEnd;
    while (compStart > 0 && Path[compStart - 1] != '/')
        compStart--;

    uint32_t compLen = compEnd - compStart;
    if (compLen == 0 || compLen > NameMax)
        return ErrIo;

    for (uint32_t i = 0; i < compLen; i++)
        NameOut[i] = Path[compStart + i];
    NameOut[compLen] = '\0';

    // Locate parent prefix
    uint32_t parentLen = compStart;
    while (parentLen > 0 && Path[parentLen - 1] == '/')
        parentLen--;

    if (parentLen == 0)
    {
        // Parent is root directory
        *ParentInodeOut = RootInode;
        return ErrOk;
    }

    return ResolvePath(Path, ParentInodeOut, nullptr, parentLen);
}

} // namespace Fs2

using Fs2::ResolvePath;
using Fs2::ResolveParent;

extern "C" {

// Fill Out with the metadata of the inode at Path.
int Fs2Stat(const char* Path, struct Fs2Stat* Out)
{
    if (Path == nullptr || Out == nullptr)
        return ErrIo;

    uint64_t inodeNo = 0;
    int status = Fs2::ResolvePath(Path, &inodeNo);
    if (status != ErrOk)
        return status;

    Fs2::Inode inode;
    status = Fs2::InodeRead(inodeNo, &inode);
    if (status != ErrOk)
        return status;

    Out->Inode = inode.InodeNo;
    Out->Mode = inode.Mode;
    Out->Flags = inode.Flags;
    Out->Uid = inode.Uid;
    Out->Gid = inode.Gid;
    Out->Links = inode.Links;
    Out->Size = inode.Size;
    Out->Blocks = inode.Blocks;
    Out->Atime = inode.Atime;
    Out->Mtime = inode.Mtime;
    Out->Ctime = inode.Ctime;
    Out->Crtime = inode.Crtime;
    for (uint32_t i = 0; i < 8; i++)
        Out->ExtTag[i] = inode.ExtTag[i];

    return ErrOk;
}

// Fill Out with directory entry Index (0-based) of the directory at Path.
int Fs2ReadDir(const char* Path, uint32_t Index, Fs2DirEntry* Out)
{
    if (Path == nullptr || Out == nullptr)
        return ErrIo;

    uint64_t dirInodeNo = 0;
    int status = Fs2::ResolvePath(Path, &dirInodeNo);
    if (status != ErrOk)
        return status;

    return Fs2::DirIterate(dirInodeNo, Index, Out);
}

// Create an empty regular file at Path with the given Mode (permissions).
// Fills ExtTag with lowercase extension (max 7 chars, NUL padded) and
// sets InodeFlagExec if extension is "exf".
int Fs2Create(const char* Path, uint16_t Mode)
{
    if (Path == nullptr)
        return ErrIo;

    uint64_t parentInode = 0;
    char name[256];
    int status = Fs2::ResolveParent(Path, &parentInode, name);
    if (status != ErrOk)
        return status;

    uint64_t fileInode = 0;
    status = Fs2::FileCreate(parentInode, name, Mode, &fileInode);
    if (status != ErrOk)
        return status;

    // Build lowercase ExtTag (max 7 chars, NUL padded)
    char tag[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const char* dot = strrchr(name, '.');
    if (dot != nullptr)
    {
        const char* ext = dot + 1;
        for (uint32_t i = 0; i < 7 && ext[i] != '\0'; i++)
        {
            char c = ext[i];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c + ('a' - 'A'));
            tag[i] = c;
        }
    }

    Fs2::Inode record;
    status = Fs2::InodeRead(fileInode, &record);
    if (status == ErrOk)
    {
        for (uint32_t i = 0; i < 8; i++)
            record.ExtTag[i] = tag[i];

        if (tag[0] == 'e' && tag[1] == 'x' && tag[2] == 'f' && tag[3] == '\0')
            record.Flags |= Fs2::InodeFlagExec;

        record.Ctime = Fs2Now();
        Fs2::InodeWrite(fileInode, &record);
    }

    return ErrOk;
}

// Create an empty directory at Path with given Mode.
int Fs2Mkdir(const char* Path, uint16_t Mode)
{
    if (Path == nullptr)
        return ErrIo;

    uint64_t parentInode = 0;
    char name[256];
    int status = Fs2::ResolveParent(Path, &parentInode, name);
    if (status != ErrOk)
        return status;

    uint64_t childInode = 0;
    return Fs2::MakeDirectory(parentInode, name, Mode, &childInode);
}

// Remove the non-directory file at Path.
int Fs2Unlink(const char* Path)
{
    if (Path == nullptr)
        return ErrIo;

    uint64_t parentInode = 0;
    char name[256];
    int status = Fs2::ResolveParent(Path, &parentInode, name);
    if (status != ErrOk)
        return status;

    return Fs2::FileUnlink(parentInode, name);
}

// Remove the empty directory at Path.
int Fs2Rmdir(const char* Path)
{
    if (Path == nullptr)
        return ErrIo;

    uint64_t parentInode = 0;
    char name[256];
    int status = Fs2::ResolveParent(Path, &parentInode, name);
    if (status != ErrOk)
        return status;

    return Fs2::RemoveDirectory(parentInode, name);
}

// Rename From to To within the volume.
int Fs2Rename(const char* From, const char* To)
{
    if (From == nullptr || To == nullptr)
        return ErrIo;

    uint64_t oldParent = 0;
    char oldName[256];
    int status = Fs2::ResolveParent(From, &oldParent, oldName);
    if (status != ErrOk)
        return status;

    uint64_t newParent = 0;
    char newName[256];
    status = Fs2::ResolveParent(To, &newParent, newName);
    if (status != ErrOk)
        return status;

    return Fs2::FileRename(oldParent, oldName, newParent, newName);
}

// Read Length bytes starting at Offset into Buffer.
int Fs2Read(const char* Path, uint64_t Offset, void* Buffer, uint64_t Length, uint64_t* ReadOut)
{
    if (Path == nullptr)
        return ErrIo;

    uint64_t inodeNo = 0;
    int status = Fs2::ResolvePath(Path, &inodeNo);
    if (status != ErrOk)
        return status;

    return Fs2::FileRead(inodeNo, Offset, Buffer, Length, ReadOut);
}

// Write Length bytes from Buffer into file at Offset.
int Fs2Write(const char* Path, uint64_t Offset, const void* Buffer, uint64_t Length, uint64_t* WrittenOut)
{
    if (Path == nullptr)
        return ErrIo;

    uint64_t inodeNo = 0;
    int status = Fs2::ResolvePath(Path, &inodeNo);
    if (status != ErrOk)
        return status;

    return Fs2::FileWrite(inodeNo, Offset, Buffer, Length, WrittenOut);
}

// Grow or shrink the file at Path to Size bytes.
int Fs2Truncate(const char* Path, uint64_t Size)
{
    if (Path == nullptr)
        return ErrIo;

    uint64_t inodeNo = 0;
    int status = Fs2::ResolvePath(Path, &inodeNo);
    if (status != ErrOk)
        return status;

    return Fs2::FileTruncate(inodeNo, Size);
}

// Mark file at Path as boot file: requires exactly one extent, sets
// InodeFlagContiguous, and writes BootFileBlock, BootFileBlocks, and
// BootFileSize into the superblock.
int Fs2SetBoot(const char* Path)
{
    if (Path == nullptr)
        return ErrIo;

    uint64_t inodeNo = 0;
    int status = Fs2::ResolvePath(Path, &inodeNo);
    if (status != ErrOk)
        return status;

    Fs2::Inode record;
    status = Fs2::InodeRead(inodeNo, &record);
    if (status != ErrOk)
        return status;

    uint16_t type = (record.Mode & Fs2::ModeTypeMask) >> Fs2::ModeTypeShift;
    if (type != Fs2::TypeFile)
        return ErrIo;

    if ((record.Flags & Fs2::InodeFlagImmutable) != 0)
        return ErrBusy;

    // Boot file must have exactly one extent
    if (record.ExtentCount != 1 || record.Extents[0].Count == 0 || record.IndirectBlock != 0)
        return ErrIo;

    record.Flags |= Fs2::InodeFlagContiguous;
    record.Ctime = Fs2Now();
    status = Fs2::InodeWrite(inodeNo, &record);
    if (status != ErrOk)
        return status;

    Fs2::Superblock* sb = Fs2::SuperGet();
    if (sb == nullptr)
        return ErrIo;

    sb->BootFileBlock = record.Extents[0].DiskBlock;
    sb->BootFileBlocks = record.Extents[0].Count;
    sb->BootFileSize = record.Size;

    return Fs2Sync();
}

} // extern "C"

