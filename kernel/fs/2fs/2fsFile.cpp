/* SPDX-License-Identifier: BSD-3-Clause */
// 2fs regular file management (docs/2fs.md): block-by-block FileRead and
// FileWrite, allocating and zero-filling missing blocks when writing past EOF,
// updating Size and Mtime; FileTruncate growing or shrinking files; FileCreate
// for new regular files; FileUnlink decrementing link count and freeing
// data blocks and inode upon reaching zero; and FileRename within the volume,
// replacing file targets while rejecting non-empty directory targets.
// Enforces the InodeFlagImmutable flag (ErrBusy) and verifies file vs dir
// types according to each operation.
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
int MapFileBlock(const Inode* Record, uint64_t FileBlock, uint64_t* DiskOut);
int AppendExtent(Inode* Record, uint64_t DiskBlock, uint32_t Count, uint64_t FileBlock);
int AppendExtent(Inode* Record, uint64_t DiskBlock, uint32_t Count);
int FreeInodeData(Inode* Record);
int GrowInode(Inode* Record, uint64_t NewSize);
int ShrinkInode(Inode* Record, uint64_t NewSize);

// Declarations of directory helpers from 2fsDir.cpp
int ValidateName(const char* Name);
int DirLookup(const Inode* DirInode, const char* Name, uint64_t* OutInode, uint8_t* OutType);
int DirLookup(uint64_t DirInodeNo, const char* Name, uint64_t* OutInode, uint8_t* OutType);
int DirAdd(Inode* Dir, const char* Name, uint8_t FileType, uint64_t InodeNo);
int DirAdd(uint64_t DirInodeNo, const char* Name, uint8_t FileType, uint64_t InodeNo);
int DirRemove(Inode* Dir, const char* Name);
int DirRemove(uint64_t DirInodeNo, const char* Name);
int DirIsEmpty(const Inode* Dir, bool* IsEmptyOut);

// Read Length bytes starting at Offset into Buffer.
// Holes in sparse files read back as zeroes.
int FileRead(const Inode* Record, uint64_t Offset, void* Buffer, uint64_t Length, uint64_t* ReadOut = nullptr)
{
    if (ReadOut != nullptr)
        *ReadOut = 0;

    if (Record == nullptr || (Length > 0 && Buffer == nullptr))
        return ErrIo;

    uint16_t type = (Record->Mode & ModeTypeMask) >> ModeTypeShift;
    if (type != TypeFile)
        return ErrIo;

    if (Offset >= Record->Size || Length == 0)
        return ErrOk; // EOF or zero length

    uint64_t toRead = Length;
    if (Offset + toRead > Record->Size)
        toRead = Record->Size - Offset;

    uint8_t* dest = static_cast<uint8_t*>(Buffer);
    uint64_t bytesRead = 0;
    uint64_t pos = Offset;
    uint64_t end = Offset + toRead;

    while (pos < end)
    {
        uint64_t fileBlock = pos / BlockSize;
        uint64_t blockOffset = pos % BlockSize;
        uint64_t chunk = BlockSize - blockOffset;
        if (chunk > end - pos)
            chunk = end - pos;

        uint64_t diskBlock = 0;
        int mapStatus = MapFileBlock(Record, fileBlock, &diskBlock);
        if (mapStatus != ErrOk)
        {
            // Unmapped hole: reads back as zero
            memset(dest + bytesRead, 0, chunk);
        }
        else
        {
            uint8_t* blockBuffer = nullptr;
            int status = GetBlock(diskBlock, &blockBuffer);
            if (status != ErrOk)
                return status;

            memcpy(dest + bytesRead, blockBuffer + blockOffset, chunk);
            PutBlock(diskBlock);
        }

        pos += chunk;
        bytesRead += chunk;
    }

    if (ReadOut != nullptr)
        *ReadOut = bytesRead;

    return ErrOk;
}

int FileRead(uint64_t InodeNo, uint64_t Offset, void* Buffer, uint64_t Length, uint64_t* ReadOut = nullptr)
{
    Inode record;
    int status = InodeRead(InodeNo, &record);
    if (status != ErrOk)
        return status;

    return FileRead(&record, Offset, Buffer, Length, ReadOut);
}

// Write Length bytes from Buffer into the file at Offset.
// Allocates missing blocks when writing past EOF, zero-filling gaps.
// Updates Size, Mtime, and persists the inode. Enforces InodeFlagImmutable (ErrBusy).
int FileWrite(Inode* Record, uint64_t Offset, const void* Buffer, uint64_t Length, uint64_t* WrittenOut = nullptr)
{
    if (WrittenOut != nullptr)
        *WrittenOut = 0;

    if (Record == nullptr || (Length > 0 && Buffer == nullptr))
        return ErrIo;

    if ((Record->Flags & InodeFlagImmutable) != 0)
        return ErrBusy;

    uint16_t type = (Record->Mode & ModeTypeMask) >> ModeTypeShift;
    if (type != TypeFile)
        return ErrIo;

    if (Length == 0)
        return ErrOk;

    // If writing past EOF, zero any uninitialized space in the last existing block
    // and allocate intervening blocks (filling holes with zeroes)
    if (Offset > Record->Size)
    {
        if (Record->Size > 0 && (Record->Size % BlockSize) != 0)
        {
            uint64_t lastBlockIdx = Record->Size / BlockSize;
            uint64_t lastBlockOffset = Record->Size % BlockSize;
            uint64_t diskBlock = 0;
            if (MapFileBlock(Record, lastBlockIdx, &diskBlock) == ErrOk)
            {
                uint8_t* blockBuffer = nullptr;
                if (GetBlock(diskBlock, &blockBuffer) == ErrOk)
                {
                    memset(blockBuffer + lastBlockOffset, 0, BlockSize - lastBlockOffset);
                    MarkDirty(diskBlock);
                    PutBlock(diskBlock);
                }
            }
        }

        int growStatus = GrowInode(Record, Offset);
        if (growStatus != ErrOk)
            return growStatus;
    }

    const uint8_t* src = static_cast<const uint8_t*>(Buffer);
    uint64_t bytesWritten = 0;
    uint64_t pos = Offset;
    uint64_t end = Offset + Length;

    while (pos < end)
    {
        uint64_t fileBlock = pos / BlockSize;
        uint64_t blockOffset = pos % BlockSize;
        uint64_t chunk = BlockSize - blockOffset;
        if (chunk > end - pos)
            chunk = end - pos;

        uint64_t diskBlock = 0;
        int mapStatus = MapFileBlock(Record, fileBlock, &diskBlock);
        if (mapStatus != ErrOk)
        {
            int status = AllocBlock(&diskBlock);
            if (status != ErrOk)
                return status;

            uint8_t* newBuffer = nullptr;
            status = NewBlock(diskBlock, &newBuffer);
            if (status != ErrOk)
            {
                FreeBlockRange(diskBlock, 1);
                return status;
            }

            status = AppendExtent(Record, diskBlock, 1, fileBlock);
            if (status != ErrOk)
            {
                PutBlock(diskBlock);
                FreeBlockRange(diskBlock, 1);
                return status;
            }

            Record->Blocks++;
            memcpy(newBuffer + blockOffset, src + bytesWritten, chunk);
            MarkDirty(diskBlock);
            PutBlock(diskBlock);
        }
        else
        {
            uint8_t* blockBuffer = nullptr;
            int status = GetBlock(diskBlock, &blockBuffer);
            if (status != ErrOk)
                return status;

            memcpy(blockBuffer + blockOffset, src + bytesWritten, chunk);
            MarkDirty(diskBlock);
            PutBlock(diskBlock);
        }

        pos += chunk;
        bytesWritten += chunk;
    }

    if (Offset + Length > Record->Size)
        Record->Size = Offset + Length;

    Record->Mtime = Fs2Now();
    Record->Ctime = Record->Mtime;

    int status = InodeWrite(Record->InodeNo, Record);
    if (status != ErrOk)
        return status;

    if (WrittenOut != nullptr)
        *WrittenOut = bytesWritten;

    return ErrOk;
}

int FileWrite(uint64_t InodeNo, uint64_t Offset, const void* Buffer, uint64_t Length, uint64_t* WrittenOut = nullptr)
{
    Inode record;
    int status = InodeRead(InodeNo, &record);
    if (status != ErrOk)
        return status;

    return FileWrite(&record, Offset, Buffer, Length, WrittenOut);
}

// Truncate or extend a regular file to NewSize bytes.
// Enforces InodeFlagImmutable (ErrBusy).
int FileTruncate(Inode* Record, uint64_t NewSize)
{
    if (Record == nullptr)
        return ErrIo;

    if ((Record->Flags & InodeFlagImmutable) != 0)
        return ErrBusy;

    uint16_t type = (Record->Mode & ModeTypeMask) >> ModeTypeShift;
    if (type != TypeFile)
        return ErrIo;

    if (NewSize == Record->Size)
        return ErrOk;

    if (NewSize > Record->Size)
    {
        int status = GrowInode(Record, NewSize);
        if (status != ErrOk)
            return status;

        return InodeWrite(Record->InodeNo, Record);
    }

    // Shrinking: zero trailing portion of the last kept block if boundary falls inside a block
    if ((NewSize % BlockSize) != 0)
    {
        uint64_t lastBlockIdx = NewSize / BlockSize;
        uint64_t lastBlockOffset = NewSize % BlockSize;
        uint64_t diskBlock = 0;
        if (MapFileBlock(Record, lastBlockIdx, &diskBlock) == ErrOk)
        {
            uint8_t* blockBuffer = nullptr;
            if (GetBlock(diskBlock, &blockBuffer) == ErrOk)
            {
                memset(blockBuffer + lastBlockOffset, 0, BlockSize - lastBlockOffset);
                MarkDirty(diskBlock);
                PutBlock(diskBlock);
            }
        }
    }

    int status = ShrinkInode(Record, NewSize);
    if (status != ErrOk)
        return status;

    return InodeWrite(Record->InodeNo, Record);
}

int FileTruncate(uint64_t InodeNo, uint64_t NewSize)
{
    Inode record;
    int status = InodeRead(InodeNo, &record);
    if (status != ErrOk)
        return status;

    return FileTruncate(&record, NewSize);
}

// Create a new regular file under ParentInodeNo with given Mode.
// Rejects duplicate names.
int FileCreate(uint64_t ParentInodeNo, const char* Name, uint16_t Mode, uint64_t* InodeOut = nullptr)
{
    if (Name == nullptr)
        return ErrIo;

    int valid = ValidateName(Name);
    if (valid != ErrOk)
        return valid;

    Inode parent;
    int status = InodeRead(ParentInodeNo, &parent);
    if (status != ErrOk)
        return status;

    uint16_t parentType = (parent.Mode & ModeTypeMask) >> ModeTypeShift;
    if (parentType != TypeDir)
        return ErrIo;

    if ((parent.Flags & InodeFlagImmutable) != 0)
        return ErrBusy;

    uint64_t existing = 0;
    if (DirLookup(&parent, Name, &existing, nullptr) == ErrOk)
        return ErrIo; // File already exists

    // Extract file extension for ExtTag
    const char* dot = strrchr(Name, '.');
    const char* ext = (dot != nullptr) ? (dot + 1) : "";

    uint64_t fileInodeNo = 0;
    uint16_t fileMode = static_cast<uint16_t>((Mode & ModePermMask) | (TypeFile << ModeTypeShift));
    status = InodeCreate(fileMode, 0, ext, &fileInodeNo);
    if (status != ErrOk)
        return status;

    status = DirAdd(&parent, Name, TypeFile, fileInodeNo);
    if (status != ErrOk)
    {
        FreeInode(fileInodeNo);
        return status;
    }

    if (InodeOut != nullptr)
        *InodeOut = fileInodeNo;

    return ErrOk;
}

// Unlink a regular file: removes entry from parent directory, decrements link count,
// and frees inode and data blocks when links reach 0. Rejects directories and immutable files.
int FileUnlink(uint64_t ParentInodeNo, const char* Name)
{
    if (Name == nullptr)
        return ErrIo;

    int valid = ValidateName(Name);
    if (valid != ErrOk)
        return valid;

    Inode parent;
    int status = InodeRead(ParentInodeNo, &parent);
    if (status != ErrOk)
        return status;

    uint16_t parentType = (parent.Mode & ModeTypeMask) >> ModeTypeShift;
    if (parentType != TypeDir)
        return ErrIo;

    if ((parent.Flags & InodeFlagImmutable) != 0)
        return ErrBusy;

    uint64_t fileInodeNo = 0;
    uint8_t fileType = 0;
    status = DirLookup(&parent, Name, &fileInodeNo, &fileType);
    if (status != ErrOk)
        return status;

    // Operation is on a non-directory file
    if (fileType == TypeDir)
        return ErrIo;

    Inode file;
    status = InodeRead(fileInodeNo, &file);
    if (status != ErrOk)
        return status;

    uint16_t targetType = (file.Mode & ModeTypeMask) >> ModeTypeShift;
    if (targetType == TypeDir)
        return ErrIo;

    if ((file.Flags & InodeFlagImmutable) != 0)
        return ErrBusy;

    status = DirRemove(&parent, Name);
    if (status != ErrOk)
        return status;

    if (file.Links > 0)
        file.Links--;

    if (file.Links == 0)
    {
        FreeInodeData(&file);
        FreeInode(fileInodeNo);
    }
    else
    {
        file.Ctime = Fs2Now();
        InodeWrite(fileInodeNo, &file);
    }

    return ErrOk;
}

// Rename within the volume: replaces destination if file, never non-empty directory.
// Enforces InodeFlagImmutable (ErrBusy) on source, destination, and directories.
int FileRename(uint64_t OldParentInodeNo, const char* OldName, uint64_t NewParentInodeNo, const char* NewName)
{
    if (OldName == nullptr || NewName == nullptr)
        return ErrIo;

    int valid = ValidateName(OldName);
    if (valid != ErrOk)
        return valid;

    valid = ValidateName(NewName);
    if (valid != ErrOk)
        return valid;

    Inode oldParent;
    int status = InodeRead(OldParentInodeNo, &oldParent);
    if (status != ErrOk)
        return status;

    uint16_t oldParentType = (oldParent.Mode & ModeTypeMask) >> ModeTypeShift;
    if (oldParentType != TypeDir)
        return ErrIo;

    if ((oldParent.Flags & InodeFlagImmutable) != 0)
        return ErrBusy;

    Inode newParent;
    if (NewParentInodeNo == OldParentInodeNo)
    {
        newParent = oldParent;
    }
    else
    {
        status = InodeRead(NewParentInodeNo, &newParent);
        if (status != ErrOk)
            return status;

        uint16_t newParentType = (newParent.Mode & ModeTypeMask) >> ModeTypeShift;
        if (newParentType != TypeDir)
            return ErrIo;

        if ((newParent.Flags & InodeFlagImmutable) != 0)
            return ErrBusy;
    }

    uint64_t sourceInodeNo = 0;
    uint8_t sourceType = 0;
    status = DirLookup(&oldParent, OldName, &sourceInodeNo, &sourceType);
    if (status != ErrOk)
        return status;

    Inode source;
    status = InodeRead(sourceInodeNo, &source);
    if (status != ErrOk)
        return status;

    if ((source.Flags & InodeFlagImmutable) != 0)
        return ErrBusy;

    if (OldParentInodeNo == NewParentInodeNo && strcmp(OldName, NewName) == 0)
        return ErrOk; // No-op rename to same name

    // Check if destination exists
    uint64_t destInodeNo = 0;
    uint8_t destType = 0;
    int destFound = DirLookup(&newParent, NewName, &destInodeNo, &destType);

    if (destFound == ErrOk)
    {
        Inode dest;
        status = InodeRead(destInodeNo, &dest);
        if (status != ErrOk)
            return status;

        if ((dest.Flags & InodeFlagImmutable) != 0)
            return ErrBusy;

        if (destType == TypeDir)
        {
            if (sourceType != TypeDir)
                return ErrIo; // Cannot replace directory with non-directory

            bool empty = false;
            DirIsEmpty(&dest, &empty);
            if (!empty)
                return ErrIo; // Cannot replace non-empty directory

            status = DirRemove(&newParent, NewName);
            if (status != ErrOk)
                return status;

            if (newParent.Links > 1)
                newParent.Links--;
            InodeWrite(NewParentInodeNo, &newParent);

            FreeInodeData(&dest);
            FreeInode(destInodeNo);
        }
        else
        {
            if (sourceType == TypeDir)
                return ErrIo; // Cannot replace non-directory with directory

            status = DirRemove(&newParent, NewName);
            if (status != ErrOk)
                return status;

            if (dest.Links > 0)
                dest.Links--;

            if (dest.Links == 0)
            {
                FreeInodeData(&dest);
                FreeInode(destInodeNo);
            }
            else
            {
                dest.Ctime = Fs2Now();
                InodeWrite(destInodeNo, &dest);
            }
        }

        // Reload parent in case entries/links changed
        InodeRead(NewParentInodeNo, &newParent);
        if (OldParentInodeNo == NewParentInodeNo)
            oldParent = newParent;
    }

    // Add entry under NewName in newParent
    status = DirAdd(&newParent, NewName, sourceType, sourceInodeNo);
    if (status != ErrOk)
        return status;

    // Remove entry OldName from oldParent
    status = DirRemove(&oldParent, OldName);
    if (status != ErrOk)
        return status;

    // If a directory was moved to a different parent, update its ".." entry and adjust parent links
    if (sourceType == TypeDir && OldParentInodeNo != NewParentInodeNo)
    {
        uint64_t diskBlock = 0;
        if (MapFileBlock(&source, 0, &diskBlock) == ErrOk)
        {
            uint8_t* buffer = nullptr;
            if (GetBlock(diskBlock, &buffer) == ErrOk)
            {
                DirEntryHeader* dotdot = reinterpret_cast<DirEntryHeader*>(buffer + 24);
                if (dotdot->NameLen == 2 && dotdot->Name[0] == '.' && dotdot->Name[1] == '.')
                {
                    dotdot->InodeNo = NewParentInodeNo;
                    MarkDirty(diskBlock);
                }
                PutBlock(diskBlock);
            }
        }

        if (oldParent.Links > 1)
        {
            oldParent.Links--;
            InodeWrite(OldParentInodeNo, &oldParent);
        }

        newParent.Links++;
        InodeWrite(NewParentInodeNo, &newParent);
    }

    source.Ctime = Fs2Now();
    return InodeWrite(sourceInodeNo, &source);
}

int FileRename(uint64_t ParentInodeNo, const char* OldName, const char* NewName)
{
    return FileRename(ParentInodeNo, OldName, ParentInodeNo, NewName);
}

} // namespace Fs2

