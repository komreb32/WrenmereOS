/* SPDX-License-Identifier: BSD-3-Clause */
// 2fs directory management (docs/2fs.md): FNV-1a 32-bit name hashing,
// DirLookup with hash-first comparison, DirAdd with hole reuse, occupied
// entry split and block allocation, DirRemove with tail merging and hole
// preservation, DirIsEmpty, DirIterate by index, MakeDirectory with "."
// and ".." plus parent link adjustments, RemoveDirectory on empty targets,
// and ValidateName. No dynamic memory is used; every operation returns
// an errors.h code.
#include "2fsLayout.h"
#include <fs2.h>
#include <lib/string.h>

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

// 32-bit FNV-1a hash over Name for Length bytes, matching tools/2fs.py
uint32_t Fnv1a(const char* Name, uint32_t Length)
{
    uint32_t hash = 2166136261u;
    for (uint32_t index = 0; index < Length; index++)
    {
        hash = (hash ^ static_cast<uint8_t>(Name[index])) * 16777619u;
    }
    return hash;
}

uint32_t Fnv1a(const char* Name)
{
    if (Name == nullptr)
        return 2166136261u;
    return Fnv1a(Name, strlen(Name));
}

uint32_t HashName(const char* Name, uint32_t Length)
{
    return Fnv1a(Name, Length);
}

uint32_t HashName(const char* Name)
{
    return Fnv1a(Name);
}

// Name validation: 1..255 bytes, no '/' and no NUL, neither "." nor ".."
int ValidateName(const char* Name)
{
    if (Name == nullptr)
        return ErrIo;

    uint32_t len = 0;
    while (Name[len] != '\0')
    {
        if (Name[len] == '/')
            return ErrIo;
        len++;
        if (len > NameMax)
            return ErrIo;
    }

    if (len == 0)
        return ErrIo;

    if (len == 1 && Name[0] == '.')
        return ErrIo;

    if (len == 2 && Name[0] == '.' && Name[1] == '.')
        return ErrIo;

    return ErrOk;
}

bool IsValidName(const char* Name)
{
    return ValidateName(Name) == ErrOk;
}

// Find an entry in DirInode by name: compares 32-bit FNV-1a hash first,
// then byte-by-byte string comparison.
int DirLookup(const Inode* DirInode, const char* Name, uint64_t* OutInode, uint8_t* OutType)
{
    if (DirInode == nullptr || Name == nullptr)
        return ErrIo;

    uint16_t type = (DirInode->Mode & ModeTypeMask) >> ModeTypeShift;
    if (type != TypeDir)
        return ErrIo;

    uint32_t nameLen = strlen(Name);
    if (nameLen == 0 || nameLen > NameMax)
        return ErrIo;

    uint32_t hash = Fnv1a(Name, nameLen);
    uint64_t numBlocks = (DirInode->Size + BlockSize - 1) / BlockSize;

    for (uint64_t blockIdx = 0; blockIdx < numBlocks; blockIdx++)
    {
        uint64_t diskBlock = 0;
        int status = MapFileBlock(DirInode, blockIdx, &diskBlock);
        if (status != ErrOk)
            continue;

        uint8_t* buffer = nullptr;
        status = GetBlock(diskBlock, &buffer);
        if (status != ErrOk)
            return status;

        uint32_t offset = 0;
        while (offset + sizeof(DirEntryHeader) <= BlockSize)
        {
            const DirEntryHeader* entry = reinterpret_cast<const DirEntryHeader*>(buffer + offset);
            if (entry->RecLen < sizeof(DirEntryHeader) || (entry->RecLen % RecLenAlign) != 0 ||
                offset + entry->RecLen > BlockSize)
            {
                break;
            }

            if (entry->InodeNo != 0)
            {
                if (entry->NameHash == hash && entry->NameLen == nameLen &&
                    memcmp(entry->Name, Name, nameLen) == 0)
                {
                    if (OutInode != nullptr)
                        *OutInode = entry->InodeNo;
                    if (OutType != nullptr)
                        *OutType = entry->FileType;

                    PutBlock(diskBlock);
                    return ErrOk;
                }
            }

            offset += entry->RecLen;
        }

        PutBlock(diskBlock);
    }

    if (OutInode != nullptr)
        *OutInode = 0;
    return ErrIo;
}

int DirLookup(uint64_t DirInodeNo, const char* Name, uint64_t* OutInode, uint8_t* OutType)
{
    Inode inode;
    int status = InodeRead(DirInodeNo, &inode);
    if (status != ErrOk)
        return status;

    return DirLookup(&inode, Name, OutInode, OutType);
}

// Add a directory entry: reuses free slots (InodeNo == 0) with sufficient RecLen,
// splits surplus RecLen of occupied records, or appends a new block to the directory.
int DirAdd(Inode* Dir, const char* Name, uint8_t FileType, uint64_t InodeNo)
{
    if (Dir == nullptr || Name == nullptr)
        return ErrIo;

    int valid = ValidateName(Name);
    if (valid != ErrOk)
        return valid;

    uint16_t type = (Dir->Mode & ModeTypeMask) >> ModeTypeShift;
    if (type != TypeDir)
        return ErrIo;

    uint32_t nameLen = strlen(Name);
    if (nameLen == 0 || nameLen > NameMax)
        return ErrIo;

    uint64_t existingInode = 0;
    if (DirLookup(Dir, Name, &existingInode, nullptr) == ErrOk)
        return ErrIo; // Duplicate entry

    uint16_t neededLen = static_cast<uint16_t>(
        (sizeof(DirEntryHeader) + nameLen + (RecLenAlign - 1)) & ~(RecLenAlign - 1));

    uint64_t numBlocks = (Dir->Size + BlockSize - 1) / BlockSize;

    // Scan existing directory blocks for free holes or splittable trailing space
    for (uint64_t blockIdx = 0; blockIdx < numBlocks; blockIdx++)
    {
        uint64_t diskBlock = 0;
        int status = MapFileBlock(Dir, blockIdx, &diskBlock);
        if (status != ErrOk)
            continue;

        uint8_t* buffer = nullptr;
        status = GetBlock(diskBlock, &buffer);
        if (status != ErrOk)
            return status;

        uint32_t offset = 0;
        while (offset + sizeof(DirEntryHeader) <= BlockSize)
        {
            DirEntryHeader* entry = reinterpret_cast<DirEntryHeader*>(buffer + offset);
            if (entry->RecLen < sizeof(DirEntryHeader) || (entry->RecLen % RecLenAlign) != 0 ||
                offset + entry->RecLen > BlockSize)
            {
                break;
            }

            // Case 1: Slot marked free (hole)
            if (entry->InodeNo == 0 && entry->RecLen >= neededLen)
            {
                entry->InodeNo = InodeNo;
                entry->NameLen = static_cast<uint8_t>(nameLen);
                entry->FileType = FileType;
                entry->NameHash = Fnv1a(Name, nameLen);
                memcpy(entry->Name, Name, nameLen);

                status = MarkDirty(diskBlock);
                int released = PutBlock(diskBlock);
                if (status != ErrOk)
                    return status;
                if (released != ErrOk)
                    return released;

                Dir->Mtime = Fs2Now();
                Dir->Ctime = Dir->Mtime;
                return InodeWrite(Dir->InodeNo, Dir);
            }

            // Case 2: Occupied record with spare padding up to the end of its record
            if (entry->InodeNo != 0)
            {
                uint16_t usedLen = static_cast<uint16_t>(
                    (sizeof(DirEntryHeader) + entry->NameLen + (RecLenAlign - 1)) & ~(RecLenAlign - 1));
                if (entry->RecLen >= usedLen + neededLen)
                {
                    uint16_t oldRecLen = entry->RecLen;
                    entry->RecLen = usedLen;

                    DirEntryHeader* newEntry = reinterpret_cast<DirEntryHeader*>(buffer + offset + usedLen);
                    newEntry->InodeNo = InodeNo;
                    newEntry->RecLen = oldRecLen - usedLen;
                    newEntry->NameLen = static_cast<uint8_t>(nameLen);
                    newEntry->FileType = FileType;
                    newEntry->NameHash = Fnv1a(Name, nameLen);
                    memcpy(newEntry->Name, Name, nameLen);

                    status = MarkDirty(diskBlock);
                    int released = PutBlock(diskBlock);
                    if (status != ErrOk)
                        return status;
                    if (released != ErrOk)
                        return released;

                    Dir->Mtime = Fs2Now();
                    Dir->Ctime = Dir->Mtime;
                    return InodeWrite(Dir->InodeNo, Dir);
                }
            }

            offset += entry->RecLen;
        }

        PutBlock(diskBlock);
    }

    // No existing block had room: allocate a new directory block
    uint64_t newDiskBlock = 0;
    int status = AllocBlock(&newDiskBlock);
    if (status != ErrOk)
        return status;

    uint8_t* buffer = nullptr;
    status = NewBlock(newDiskBlock, &buffer);
    if (status != ErrOk)
    {
        FreeBlockRange(newDiskBlock, 1);
        return status;
    }

    DirEntryHeader* entry = reinterpret_cast<DirEntryHeader*>(buffer);
    entry->InodeNo = InodeNo;
    entry->RecLen = static_cast<uint16_t>(BlockSize); // Covers entire 4096-byte block
    entry->NameLen = static_cast<uint8_t>(nameLen);
    entry->FileType = FileType;
    entry->NameHash = Fnv1a(Name, nameLen);
    memcpy(entry->Name, Name, nameLen);

    status = MarkDirty(newDiskBlock);
    int released = PutBlock(newDiskBlock);
    if (status != ErrOk)
    {
        FreeBlockRange(newDiskBlock, 1);
        return status;
    }
    if (released != ErrOk)
    {
        FreeBlockRange(newDiskBlock, 1);
        return released;
    }

    uint64_t fileBlock = (Dir->Size == 0) ? 0 : (Dir->Size / BlockSize);
    status = AppendExtent(Dir, newDiskBlock, 1, fileBlock);
    if (status != ErrOk)
    {
        FreeBlockRange(newDiskBlock, 1);
        return status;
    }

    Dir->Blocks++;
    Dir->Size += BlockSize;
    Dir->Mtime = Fs2Now();
    Dir->Ctime = Dir->Mtime;
    return InodeWrite(Dir->InodeNo, Dir);
}

int DirAdd(uint64_t DirInodeNo, const char* Name, uint8_t FileType, uint64_t InodeNo)
{
    Inode dir;
    int status = InodeRead(DirInodeNo, &dir);
    if (status != ErrOk)
        return status;

    return DirAdd(&dir, Name, FileType, InodeNo);
}

// Remove an entry: merges its RecLen with the preceding entry in the block,
// or marks the slot free (InodeNo = 0) if it is the first entry in the block.
int DirRemove(Inode* Dir, const char* Name)
{
    if (Dir == nullptr || Name == nullptr)
        return ErrIo;

    int valid = ValidateName(Name);
    if (valid != ErrOk)
        return valid;

    uint16_t type = (Dir->Mode & ModeTypeMask) >> ModeTypeShift;
    if (type != TypeDir)
        return ErrIo;

    uint32_t nameLen = strlen(Name);
    if (nameLen == 0 || nameLen > NameMax)
        return ErrIo;

    uint32_t hash = Fnv1a(Name, nameLen);
    uint64_t numBlocks = (Dir->Size + BlockSize - 1) / BlockSize;

    for (uint64_t blockIdx = 0; blockIdx < numBlocks; blockIdx++)
    {
        uint64_t diskBlock = 0;
        int status = MapFileBlock(Dir, blockIdx, &diskBlock);
        if (status != ErrOk)
            continue;

        uint8_t* buffer = nullptr;
        status = GetBlock(diskBlock, &buffer);
        if (status != ErrOk)
            return status;

        DirEntryHeader* prevEntry = nullptr;
        uint32_t offset = 0;
        while (offset + sizeof(DirEntryHeader) <= BlockSize)
        {
            DirEntryHeader* currEntry = reinterpret_cast<DirEntryHeader*>(buffer + offset);
            if (currEntry->RecLen < sizeof(DirEntryHeader) || (currEntry->RecLen % RecLenAlign) != 0 ||
                offset + currEntry->RecLen > BlockSize)
            {
                break;
            }

            if (currEntry->InodeNo != 0)
            {
                if (currEntry->NameHash == hash && currEntry->NameLen == nameLen &&
                    memcmp(currEntry->Name, Name, nameLen) == 0)
                {
                    // Target entry found
                    if (prevEntry != nullptr)
                    {
                        // Merge RecLen with previous entry
                        prevEntry->RecLen += currEntry->RecLen;
                    }
                    else
                    {
                        // First entry in the block: mark slot free
                        currEntry->InodeNo = 0;
                    }

                    status = MarkDirty(diskBlock);
                    int released = PutBlock(diskBlock);
                    if (status != ErrOk)
                        return status;
                    if (released != ErrOk)
                        return released;

                    Dir->Mtime = Fs2Now();
                    Dir->Ctime = Dir->Mtime;
                    return InodeWrite(Dir->InodeNo, Dir);
                }
            }

            prevEntry = currEntry;
            offset += currEntry->RecLen;
        }

        PutBlock(diskBlock);
    }

    return ErrIo; // Not found
}

int DirRemove(uint64_t DirInodeNo, const char* Name)
{
    Inode dir;
    int status = InodeRead(DirInodeNo, &dir);
    if (status != ErrOk)
        return status;

    return DirRemove(&dir, Name);
}

// Check if a directory is empty (contains only "." and "..")
int DirIsEmpty(const Inode* Dir, bool* IsEmptyOut)
{
    if (Dir == nullptr || IsEmptyOut == nullptr)
        return ErrIo;

    uint16_t type = (Dir->Mode & ModeTypeMask) >> ModeTypeShift;
    if (type != TypeDir)
        return ErrIo;

    *IsEmptyOut = true;
    uint64_t numBlocks = (Dir->Size + BlockSize - 1) / BlockSize;

    for (uint64_t blockIdx = 0; blockIdx < numBlocks; blockIdx++)
    {
        uint64_t diskBlock = 0;
        int status = MapFileBlock(Dir, blockIdx, &diskBlock);
        if (status != ErrOk)
            continue;

        uint8_t* buffer = nullptr;
        status = GetBlock(diskBlock, &buffer);
        if (status != ErrOk)
            return status;

        uint32_t offset = 0;
        while (offset + sizeof(DirEntryHeader) <= BlockSize)
        {
            const DirEntryHeader* entry = reinterpret_cast<const DirEntryHeader*>(buffer + offset);
            if (entry->RecLen < sizeof(DirEntryHeader) || (entry->RecLen % RecLenAlign) != 0 ||
                offset + entry->RecLen > BlockSize)
            {
                break;
            }

            if (entry->InodeNo != 0)
            {
                bool isDot = (entry->NameLen == 1 && entry->Name[0] == '.');
                bool isDotDot = (entry->NameLen == 2 && entry->Name[0] == '.' && entry->Name[1] == '.');
                if (!isDot && !isDotDot)
                {
                    *IsEmptyOut = false;
                    PutBlock(diskBlock);
                    return ErrOk;
                }
            }

            offset += entry->RecLen;
        }

        PutBlock(diskBlock);
    }

    return ErrOk;
}

int DirIsEmpty(uint64_t DirInodeNo, bool* IsEmptyOut)
{
    Inode dir;
    int status = InodeRead(DirInodeNo, &dir);
    if (status != ErrOk)
        return status;

    return DirIsEmpty(&dir, IsEmptyOut);
}

bool DirIsEmpty(const Inode* Dir)
{
    bool empty = false;
    if (DirIsEmpty(Dir, &empty) == ErrOk)
        return empty;
    return false;
}

// Iterate entries of a directory by 0-based index in on-disk order
int DirIterate(const Inode* Dir, uint32_t Index, Fs2DirEntry* Out)
{
    if (Dir == nullptr || Out == nullptr)
        return ErrIo;

    uint16_t type = (Dir->Mode & ModeTypeMask) >> ModeTypeShift;
    if (type != TypeDir)
        return ErrIo;

    uint32_t currentIndex = 0;
    uint64_t numBlocks = (Dir->Size + BlockSize - 1) / BlockSize;

    for (uint64_t blockIdx = 0; blockIdx < numBlocks; blockIdx++)
    {
        uint64_t diskBlock = 0;
        int status = MapFileBlock(Dir, blockIdx, &diskBlock);
        if (status != ErrOk)
            continue;

        uint8_t* buffer = nullptr;
        status = GetBlock(diskBlock, &buffer);
        if (status != ErrOk)
            return status;

        uint32_t offset = 0;
        while (offset + sizeof(DirEntryHeader) <= BlockSize)
        {
            const DirEntryHeader* entry = reinterpret_cast<const DirEntryHeader*>(buffer + offset);
            if (entry->RecLen < sizeof(DirEntryHeader) || (entry->RecLen % RecLenAlign) != 0 ||
                offset + entry->RecLen > BlockSize)
            {
                break;
            }

            if (entry->InodeNo != 0)
            {
                if (currentIndex == Index)
                {
                    Out->Inode = entry->InodeNo;
                    Out->FileType = entry->FileType;
                    uint32_t copyLen = entry->NameLen;
                    if (copyLen > 255)
                        copyLen = 255;
                    memcpy(Out->Name, entry->Name, copyLen);
                    Out->Name[copyLen] = '\0';

                    PutBlock(diskBlock);
                    return ErrOk;
                }
                currentIndex++;
            }

            offset += entry->RecLen;
        }

        PutBlock(diskBlock);
    }

    return ErrIo; // Index past the end of entries
}

int DirIterate(uint64_t DirInodeNo, uint32_t Index, Fs2DirEntry* Out)
{
    Inode dir;
    int status = InodeRead(DirInodeNo, &dir);
    if (status != ErrOk)
        return status;

    return DirIterate(&dir, Index, Out);
}

// Create a new directory under ParentInodeNo with "." and ".." and adjust parent Links
int MakeDirectory(uint64_t ParentInodeNo, const char* Name, uint16_t Mode, uint64_t* InodeOut)
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

    uint64_t existingInode = 0;
    if (DirLookup(&parent, Name, &existingInode, nullptr) == ErrOk)
        return ErrIo; // Already exists

    // 1. Reserve and create the child directory inode
    uint64_t childInodeNo = 0;
    uint16_t dirMode = static_cast<uint16_t>((Mode & ModePermMask) | (TypeDir << ModeTypeShift));
    status = InodeCreate(dirMode, 0, "", &childInodeNo);
    if (status != ErrOk)
        return status;

    // 2. Allocate 1 block for the child directory's initial entries
    uint64_t childBlock = 0;
    status = AllocBlock(&childBlock);
    if (status != ErrOk)
    {
        FreeInode(childInodeNo);
        return status;
    }

    // 3. Initialize block with "." and ".."
    uint8_t* buffer = nullptr;
    status = NewBlock(childBlock, &buffer);
    if (status != ErrOk)
    {
        FreeBlockRange(childBlock, 1);
        FreeInode(childInodeNo);
        return status;
    }

    // Entry 1: "." (rec_len = 24)
    DirEntryHeader* dot = reinterpret_cast<DirEntryHeader*>(buffer);
    dot->InodeNo = childInodeNo;
    dot->RecLen = 24;
    dot->NameLen = 1;
    dot->FileType = TypeDir;
    dot->NameHash = Fnv1a(".", 1);
    dot->Name[0] = '.';

    // Entry 2: ".." (rec_len = BlockSize - 24)
    DirEntryHeader* dotdot = reinterpret_cast<DirEntryHeader*>(buffer + 24);
    dotdot->InodeNo = ParentInodeNo;
    dotdot->RecLen = static_cast<uint16_t>(BlockSize - 24);
    dotdot->NameLen = 2;
    dotdot->FileType = TypeDir;
    dotdot->NameHash = Fnv1a("..", 2);
    dotdot->Name[0] = '.';
    dotdot->Name[1] = '.';

    status = MarkDirty(childBlock);
    int released = PutBlock(childBlock);
    if (status != ErrOk || released != ErrOk)
    {
        FreeBlockRange(childBlock, 1);
        FreeInode(childInodeNo);
        return (status != ErrOk) ? status : released;
    }

    // 4. Update child inode metadata: Links = 2, Size = BlockSize, Blocks = 1, Extents[0]
    Inode child;
    status = InodeRead(childInodeNo, &child);
    if (status != ErrOk)
    {
        FreeBlockRange(childBlock, 1);
        FreeInode(childInodeNo);
        return status;
    }

    child.Links = 2; // self (".") and reference in parent
    child.Size = BlockSize;
    child.Blocks = 1;
    child.ExtentCount = 1;
    child.Extents[0].FileBlock = 0;
    child.Extents[0].DiskBlock = childBlock;
    child.Extents[0].Count = 1;
    child.Extents[0].Flags = 0;
    status = InodeWrite(childInodeNo, &child);
    if (status != ErrOk)
    {
        FreeBlockRange(childBlock, 1);
        FreeInode(childInodeNo);
        return status;
    }

    // 5. Add child entry into parent directory
    status = DirAdd(&parent, Name, TypeDir, childInodeNo);
    if (status != ErrOk)
    {
        FreeInodeData(&child);
        FreeInode(childInodeNo);
        return status;
    }

    // 6. Adjust parent link count: child's ".." points to parent
    parent.Links++;
    status = InodeWrite(ParentInodeNo, &parent);
    if (status != ErrOk)
        return status;

    if (InodeOut != nullptr)
        *InodeOut = childInodeNo;

    return ErrOk;
}

// Remove an empty directory under ParentInodeNo
int RemoveDirectory(uint64_t ParentInodeNo, const char* Name)
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

    uint64_t childInodeNo = 0;
    uint8_t childType = 0;
    status = DirLookup(&parent, Name, &childInodeNo, &childType);
    if (status != ErrOk)
        return status;

    if (childType != TypeDir)
        return ErrIo; // Not a directory

    Inode child;
    status = InodeRead(childInodeNo, &child);
    if (status != ErrOk)
        return status;

    uint16_t childModeType = (child.Mode & ModeTypeMask) >> ModeTypeShift;
    if (childModeType != TypeDir)
        return ErrIo;

    // RemoveDirectory requires an empty directory
    bool empty = false;
    status = DirIsEmpty(&child, &empty);
    if (status != ErrOk)
        return status;
    if (!empty)
        return ErrIo;

    // Remove entry from parent directory
    status = DirRemove(&parent, Name);
    if (status != ErrOk)
        return status;

    // Adjust parent's link count (child's ".." was pointing to parent)
    if (parent.Links > 1)
        parent.Links--;
    status = InodeWrite(ParentInodeNo, &parent);
    if (status != ErrOk)
        return status;

    // Release child data blocks and inode
    FreeInodeData(&child);
    FreeInode(childInodeNo);

    return ErrOk;
}

} // namespace Fs2

