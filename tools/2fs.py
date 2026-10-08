#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Create, format and inspect 2fs v1 images."""

import argparse
import os
import re
import struct
import sys
import time
import uuid
import zlib
from pathlib import Path

SECTOR_SIZE = 512
SECTORS_PER_BLOCK = 8
BLOCK_SIZE = 4096
BITS_PER_BITMAP_BLOCK = BLOCK_SIZE * 8
INODES_PER_BLOCK = BLOCK_SIZE // 256
SUPERBLOCK_MAGIC = 0x4E4552572D534632
INODE_SIZE = 256
EXTENT_SIZE = 24
DIR_ENTRY_HEADER_SIZE = 16
SUPERBLOCK_CHECKSUM_OFFSET = BLOCK_SIZE - 4
INODE_CHECKSUM_OFFSET = 240
INODE_TYPE_DIR = 2
INODE_TYPE_FILE = 1
DIRENT_TYPE_DIR = 2
DIRENT_TYPE_FILE = 1
FILE_TYPE_REGULAR = 1
INODE_FLAG_EXEC = 1
INODE_FLAG_CONTIG = 1 << 4

SUPERBLOCK_STRUCT = struct.Struct("<QHHIQQQQIIQQQQQQQQQQQQ16s64sQQQI")
INODE_STRUCT = struct.Struct("<HHIIIQQQQQQIIQ8sQ")
EXTENT_STRUCT = struct.Struct("<QQII")
DIR_ENTRY_STRUCT = struct.Struct("<QHBBI")

assert BLOCK_SIZE == 4096
assert INODE_SIZE == 256
assert EXTENT_SIZE == 24
assert INODES_PER_BLOCK == 16
assert DIR_ENTRY_HEADER_SIZE == 16


class Fs2Error(Exception):
    pass


def Crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def ParseSize(value):
    match = re.fullmatch(r"\s*(\d+)\s*([KMGT]?)(?:I?B)?\s*", value, re.IGNORECASE)
    if not match:
        raise argparse.ArgumentTypeError(f"invalid size {value!r}; use 512M, 1G, or bytes")
    amount = int(match.group(1))
    suffix = match.group(2).upper()
    scale = {"": 0, "K": 1, "M": 2, "G": 3, "T": 4}[suffix]
    size = amount * (1024 ** scale)
    if size <= 0:
        raise argparse.ArgumentTypeError("size must be greater than zero")
    return size


def _ceil_div(a, b):
    return (a + b - 1) // b


def CalcGeometry(totalSectors, offsetSectors, bytesPerInode=16384):
    if totalSectors < 0 or offsetSectors < 0:
        raise Fs2Error("negative sector counts are invalid")
    if offsetSectors >= totalSectors:
        raise Fs2Error("offset leaves no room for the filesystem")
    if bytesPerInode <= 0:
        raise Fs2Error("bytes-per-inode must be positive")

    volumeSectors = totalSectors - offsetSectors
    totalBlocks = volumeSectors // SECTORS_PER_BLOCK
    if totalBlocks < 1:
        raise Fs2Error("volume is too small for 2fs")

    inodeEstimate = _ceil_div(totalBlocks * BLOCK_SIZE, bytesPerInode)
    totalInodes = _ceil_div(inodeEstimate, 16) * 16
    if totalInodes < 16:
        totalInodes = 16

    blockBitmapStart = 2
    blockBitmapBlocks = _ceil_div(totalBlocks, BITS_PER_BITMAP_BLOCK)
    inodeBitmapStart = blockBitmapStart + blockBitmapBlocks
    inodeBitmapBlocks = _ceil_div(totalInodes, BITS_PER_BITMAP_BLOCK)
    inodeTableStart = inodeBitmapStart + inodeBitmapBlocks
    inodeTableBlocks = _ceil_div(totalInodes, INODES_PER_BLOCK)
    dataStart = inodeTableStart + inodeTableBlocks
    backupBlock = totalBlocks - 1
    if dataStart >= backupBlock:
        raise Fs2Error("volume too small for metadata, root dir, and backup superblock")

    return {
        "OffsetSectors": offsetSectors,
        "TotalBlocks": totalBlocks,
        "TotalInodes": totalInodes,
        "FreeBlocks": totalBlocks - dataStart - 2,
        "FreeInodes": totalInodes - 2,
        "BlockBitmapStart": blockBitmapStart,
        "BlockBitmapBlocks": blockBitmapBlocks,
        "InodeBitmapStart": inodeBitmapStart,
        "InodeBitmapBlocks": inodeBitmapBlocks,
        "InodeTableStart": inodeTableStart,
        "InodeTableBlocks": inodeTableBlocks,
        "DataStart": dataStart,
        "BackupBlock": backupBlock,
        "RootInode": 1,
        "BootFileBlock": 0,
        "BootFileBlocks": 0,
        "BootFileSize": 0,
    }


class Superblock:
    SIZE = BLOCK_SIZE

    def __init__(self, **fields):
        self.fields = fields

    def pack(self):
        fields = self.fields
        label = fields["Label"]
        if isinstance(label, str):
            label = label.encode("utf-8")
        label = label.rstrip(b"\0")
        if b"\0" in label:
            raise Fs2Error("label must not contain a NUL byte")
        if len(label) > 63:
            raise Fs2Error("label must be <= 63 UTF-8 bytes")
        label = label.ljust(64, b"\0")
        uuid_bytes = fields["Uuid"]
        if len(uuid_bytes) != 16:
            raise Fs2Error("UUID must be exactly 16 bytes")

        block = bytearray(BLOCK_SIZE)
        SUPERBLOCK_STRUCT.pack_into(
            block,
            0,
            fields.get("Magic", SUPERBLOCK_MAGIC),
            fields.get("VersionMajor", 1),
            fields.get("VersionMinor", 0),
            fields.get("BlockSize", BLOCK_SIZE),
            fields["TotalBlocks"],
            fields["FreeBlocks"],
            fields["TotalInodes"],
            fields["FreeInodes"],
            fields.get("InodeSize", INODE_SIZE),
            fields.get("Flags", 0),
            fields["BlockBitmapStart"],
            fields["BlockBitmapBlocks"],
            fields["InodeBitmapStart"],
            fields["InodeBitmapBlocks"],
            fields["InodeTableStart"],
            fields["InodeTableBlocks"],
            fields["DataStart"],
            fields.get("RootInode", 1),
            fields["BackupBlock"],
            fields.get("BootFileBlock", 0),
            fields.get("BootFileBlocks", 0),
            fields.get("BootFileSize", 0),
            uuid_bytes,
            label,
            fields["CreatedTime"],
            fields.get("MountedTime", 0),
            fields["WrittenTime"],
            fields.get("MountCount", 0),
        )
        struct.pack_into("<I", block, SUPERBLOCK_CHECKSUM_OFFSET, Crc32(block[:SUPERBLOCK_CHECKSUM_OFFSET]))
        return bytes(block)

    @classmethod
    def unpack(cls, data):
        if len(data) != BLOCK_SIZE:
            raise Fs2Error("superblock must be exactly 4096 bytes")
        values = SUPERBLOCK_STRUCT.unpack_from(data)
        names = (
            "Magic", "VersionMajor", "VersionMinor", "BlockSize",
            "TotalBlocks", "FreeBlocks", "TotalInodes", "FreeInodes",
            "InodeSize", "Flags", "BlockBitmapStart", "BlockBitmapBlocks",
            "InodeBitmapStart", "InodeBitmapBlocks", "InodeTableStart",
            "InodeTableBlocks", "DataStart", "RootInode", "BackupBlock",
            "BootFileBlock", "BootFileBlocks", "BootFileSize", "Uuid",
            "Label", "CreatedTime", "MountedTime", "WrittenTime", "MountCount",
        )
        result = cls(**dict(zip(names, values)))
        result.fields["Checksum"] = struct.unpack_from("<I", data, SUPERBLOCK_CHECKSUM_OFFSET)[0]
        return result


class Extent:
    SIZE = EXTENT_SIZE

    def __init__(self, FileBlock=0, DiskBlock=0, Count=0, Flags=0):
        self.FileBlock = FileBlock
        self.DiskBlock = DiskBlock
        self.Count = Count
        self.Flags = Flags

    def pack(self):
        return EXTENT_STRUCT.pack(self.FileBlock, self.DiskBlock, self.Count, self.Flags)

    @classmethod
    def unpack(cls, data, offset=0):
        return cls(*EXTENT_STRUCT.unpack_from(data, offset))


class Inode:
    SIZE = INODE_SIZE

    def __init__(self, Mode=0, Flags=0, Uid=0, Gid=0, Links=0, Size=0, Blocks=0,
                 Atime=0, Mtime=0, Ctime=0, Crtime=0, ExtentCount=0, Reserved=0,
                 IndirectBlock=0, ExtTag=b"", InodeNo=0, Extents=None):
        self.Mode = Mode
        self.Flags = Flags
        self.Uid = Uid
        self.Gid = Gid
        self.Links = Links
        self.Size = Size
        self.Blocks = Blocks
        self.Atime = Atime
        self.Mtime = Mtime
        self.Ctime = Ctime
        self.Crtime = Crtime
        self.ExtentCount = ExtentCount
        self.Reserved = Reserved
        self.IndirectBlock = IndirectBlock
        self.ExtTag = ExtTag.encode("ascii") if isinstance(ExtTag, str) else ExtTag
        self.InodeNo = InodeNo
        self.Extents = list(Extents or [])
        self.Checksum = 0

    def pack(self):
        if len(self.ExtTag) > 8:
            raise Fs2Error("inode extension tag must be at most 8 bytes")
        if len(self.Extents) > 6:
            raise Fs2Error("inode can store at most 6 inline extents")
        block = bytearray(INODE_SIZE)
        INODE_STRUCT.pack_into(
            block, 0,
            self.Mode, self.Flags, self.Uid, self.Gid, self.Links,
            self.Size, self.Blocks, self.Atime, self.Mtime, self.Ctime, self.Crtime,
            self.ExtentCount, self.Reserved, self.IndirectBlock,
            self.ExtTag.ljust(8, b"\0"), self.InodeNo,
        )
        for i, ext in enumerate(self.Extents[:6]):
            block[96 + i * EXTENT_SIZE:96 + (i + 1) * EXTENT_SIZE] = ext.pack()
        struct.pack_into("<I", block, INODE_CHECKSUM_OFFSET, 0)
        self.Checksum = Crc32(block)
        struct.pack_into("<I", block, INODE_CHECKSUM_OFFSET, self.Checksum)
        return bytes(block)

    @classmethod
    def unpack(cls, data):
        if len(data) != INODE_SIZE:
            raise Fs2Error("inode must be exactly 256 bytes")
        values = INODE_STRUCT.unpack_from(data)
        extents = [Extent.unpack(data, 96 + i * EXTENT_SIZE) for i in range(6)]
        inode = cls(*values, Extents=extents)
        inode.Checksum = struct.unpack_from("<I", data, INODE_CHECKSUM_OFFSET)[0]
        return inode


class DirEntry:
    HEADER_SIZE = DIR_ENTRY_HEADER_SIZE

    def __init__(self, InodeNo, Name, FileType, RecLen=0, NameHash=None):
        self.InodeNo = InodeNo
        self.Name = Name.encode("utf-8") if isinstance(Name, str) else bytes(Name)
        self.FileType = FileType
        self.RecLen = RecLen
        self.NameHash = DirEntry.HashName(self.Name) if NameHash is None else NameHash

    @staticmethod
    def HashName(name):
        value = 2166136261
        for byte in name:
            value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
        return value

    def pack(self, rec_len=None):
        if not 1 <= len(self.Name) <= 255:
            raise Fs2Error("directory entry name must be 1..255 bytes")
        if b"\0" in self.Name or b"/" in self.Name:
            raise Fs2Error("directory names cannot contain NUL or '/'")
        min_len = (DIR_ENTRY_HEADER_SIZE + len(self.Name) + 7) & ~7
        requested_len = min_len if rec_len is None else rec_len
        if requested_len < min_len or requested_len % 8:
            raise Fs2Error("directory record length must be aligned and large enough")
        header = DIR_ENTRY_STRUCT.pack(self.InodeNo, requested_len, len(self.Name), self.FileType, self.NameHash)
        return header + self.Name + b"\0" * (requested_len - (DIR_ENTRY_HEADER_SIZE + len(self.Name)))

    @classmethod
    def unpack(cls, data, offset=0):
        if len(data) - offset < DIR_ENTRY_HEADER_SIZE:
            raise Fs2Error("directory entry header truncated")
        inode_no, rec_len, name_len, file_type, name_hash = DIR_ENTRY_STRUCT.unpack_from(data, offset)
        if rec_len < DIR_ENTRY_HEADER_SIZE or rec_len % 8:
            raise Fs2Error("invalid directory record length")
        if name_len == 0 or name_len > 255 or name_len > rec_len - DIR_ENTRY_HEADER_SIZE:
            raise Fs2Error("invalid name length in directory entry")
        name = data[offset + DIR_ENTRY_HEADER_SIZE:offset + DIR_ENTRY_HEADER_SIZE + name_len]
        return cls(inode_no, name, file_type, rec_len, name_hash)


assert Superblock.SIZE == 4096
assert Inode.SIZE == 256
assert Extent.SIZE == 24


def _set_bitmap_range(bitmap, start, count):
    end = start + count
    while start < end:
        byte_index = start >> 3
        bit_index = start & 7
        run = min(end - start, 8 - bit_index)
        bitmap[byte_index] |= ((1 << run) - 1) << bit_index
        start += run


def _clear_bitmap_range(bitmap, start, count):
    end = start + count
    while start < end:
        byte_index = start >> 3
        bit_index = start & 7
        run = min(end - start, 8 - bit_index)
        bitmap[byte_index] &= ~(((1 << run) - 1) << bit_index)
        start += run


class Image:
    def __init__(self, image_path, offset_sectors=0, volume_blocks=None):
        self.Path = Path(image_path)
        self.File = self.Path.open("r+b")
        self.OffsetSectors = offset_sectors
        self.VolumeBlocks = volume_blocks
        self.TotalSectors = self.Path.stat().st_size // SECTOR_SIZE

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

    def close(self):
        if not self.File.closed:
            self.File.close()

    def ReadSectors(self, lba, count):
        if lba < 0 or count < 0 or lba + count > self.TotalSectors:
            raise Fs2Error("sector read outside image")
        self.File.seek(lba * SECTOR_SIZE)
        data = self.File.read(count * SECTOR_SIZE)
        if len(data) != count * SECTOR_SIZE:
            raise Fs2Error("short sector read")
        return data

    def WriteSectors(self, lba, data):
        if len(data) % SECTOR_SIZE:
            raise Fs2Error("sector writes must be a multiple of 512 bytes")
        count = len(data) // SECTOR_SIZE
        if lba < 0 or lba + count > self.TotalSectors:
            raise Fs2Error("sector write outside image")
        self.File.seek(lba * SECTOR_SIZE)
        self.File.write(data)

    def ReadBlock(self, block_number):
        if block_number < 0:
            raise Fs2Error("block number must not be negative")
        if self.VolumeBlocks is not None and block_number >= self.VolumeBlocks:
            raise Fs2Error("block number outside the filesystem volume")
        lba = self.OffsetSectors + block_number * SECTORS_PER_BLOCK
        return self.ReadSectors(lba, SECTORS_PER_BLOCK)

    def WriteBlock(self, block_number, data):
        if block_number < 0:
            raise Fs2Error("block number must not be negative")
        if self.VolumeBlocks is not None and block_number >= self.VolumeBlocks:
            raise Fs2Error("block number outside the filesystem volume")
        if len(data) != BLOCK_SIZE:
            raise Fs2Error("block writes must be exactly 4096 bytes")
        lba = self.OffsetSectors + block_number * SECTORS_PER_BLOCK
        self.WriteSectors(lba, data)

    def ZeroBlocks(self, start, count):
        if count < 0:
            raise Fs2Error("block count must not be negative")
        chunk = b"\0" * (1024 * 1024)
        while count:
            part = min(count, len(chunk) // BLOCK_SIZE)
            self.WriteSectors(self.OffsetSectors + start * SECTORS_PER_BLOCK, chunk[:part * BLOCK_SIZE])
            start += part
            count -= part

    def WriteBlocks(self, start, data):
        if len(data) % BLOCK_SIZE:
            raise Fs2Error("block writes must be a multiple of 4096 bytes")
        lba = self.OffsetSectors + start * SECTORS_PER_BLOCK
        self.WriteSectors(lba, data)


class Volume:
    def __init__(self, image_path, offset_sectors=2048):
        self.Image = Image(image_path, offset_sectors)
        self.OffsetSectors = offset_sectors
        self.Super = self._ReadSuperblock()
        self.Image.VolumeBlocks = self.Super.fields["TotalBlocks"]
        self.BlockBitmap = self._ReadBitmap(self.Super.fields["BlockBitmapStart"], self.Super.fields["BlockBitmapBlocks"])
        self.InodeBitmap = self._ReadBitmap(self.Super.fields["InodeBitmapStart"], self.Super.fields["InodeBitmapBlocks"])
        self.FreeBlocksCount = int(self.Super.fields["FreeBlocks"])
        self.FreeInodesCount = int(self.Super.fields["FreeInodes"])

    def _ReadSuperblock(self):
        block = self.Image.ReadBlock(0)
        sb = Superblock.unpack(block)
        if sb.fields.get("Magic") != SUPERBLOCK_MAGIC:
            raise Fs2Error("invalid 2fs magic")
        if sb.fields.get("BlockSize") != BLOCK_SIZE:
            raise Fs2Error("unsupported block size")
        expected = Crc32(block[:SUPERBLOCK_CHECKSUM_OFFSET])
        if sb.fields.get("Checksum") != expected:
            raise Fs2Error("superblock checksum mismatch")
        return sb

    def _ReadBitmap(self, start, blocks):
        bitmap = bytearray(blocks * BLOCK_SIZE)
        for i in range(blocks):
            payload = self.Image.ReadBlock(start + i)
            bitmap[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE] = payload
        return bitmap

    def _WriteBitmap(self, start, bitmap):
        for i in range(0, len(bitmap), BLOCK_SIZE):
            chunk = bitmap[i:i + BLOCK_SIZE]
            self.Image.WriteBlock(start + i // BLOCK_SIZE, chunk)

    @staticmethod
    def _TestBit(bitmap, bit):
        return (bitmap[bit // 8] >> (bit % 8)) & 1

    @staticmethod
    def _SetBit(bitmap, bit):
        bitmap[bit // 8] |= 1 << (bit % 8)

    @staticmethod
    def _ClearBit(bitmap, bit):
        bitmap[bit // 8] &= ~(1 << (bit % 8))

    def _FindFreeBlocksScattered(self, bitmap, count, total_blocks):
        found = []
        for index in range(total_blocks):
            if not self._TestBit(bitmap, index):
                found.append(index)
                if len(found) == count:
                    return found
        return None

    def _FindFreeRange(self, bitmap, count, contiguous=False):
        total_bits = len(bitmap) * 8
        total_blocks = min(total_bits, int(self.Super.fields["TotalBlocks"]))
        if not contiguous:
            # Non-contiguous allocation: any free blocks satisfy the request,
            # even if they are scattered across the volume.
            return self._FindFreeBlocksScattered(bitmap, count, total_blocks)
        index = 0
        while index + count <= total_blocks:
            if self._TestBit(bitmap, index):
                index += 1
                continue
            if count == 1:
                return index
            run = 1
            pos = index + 1
            while pos < total_blocks and run < count and not self._TestBit(bitmap, pos):
                run += 1
                pos += 1
            if run == count:
                return index
            index = pos if pos > index else index + 1
        return None

    def AllocBlocks(self, count, contiguous=False):
        if count <= 0:
            return []
        start = self._FindFreeRange(self.BlockBitmap, count, contiguous)
        if start is None:
            if contiguous:
                raise Fs2Error("no contiguous block range available")
            raise Fs2Error("no free blocks available")
        result = []
        if contiguous:
            for i in range(count):
                block = start + i
                self._SetBit(self.BlockBitmap, block)
            result.append(Extent(FileBlock=0, DiskBlock=start, Count=count))
        else:
            for block in start:
                self._SetBit(self.BlockBitmap, block)
                result.append(Extent(FileBlock=0, DiskBlock=block, Count=1))
        self.FreeBlocksCount -= count
        return result

    def FreeBlocks(self, extents):
        if not extents:
            return
        for extent in extents:
            if extent is None or extent.Count <= 0:
                continue
            for i in range(extent.Count):
                block = extent.DiskBlock + i
                if block < 0 or block >= self.Super.fields["TotalBlocks"]:
                    continue
                if self._TestBit(self.BlockBitmap, block):
                    self._ClearBit(self.BlockBitmap, block)
                    self.FreeBlocksCount += 1

    def AllocInode(self):
        for inode_no in range(1, self.Super.fields["TotalInodes"]):
            if not self._TestBit(self.InodeBitmap, inode_no):
                self._SetBit(self.InodeBitmap, inode_no)
                self.FreeInodesCount -= 1
                return inode_no
        raise Fs2Error("no free inodes available")

    def FreeInode(self, inode_no):
        if inode_no <= 0 or inode_no >= self.Super.fields["TotalInodes"]:
            raise Fs2Error("invalid inode number")
        self._ClearBit(self.InodeBitmap, inode_no)
        self.FreeInodesCount += 1

    def _InodeTableBlock(self, inode_no):
        return self.Super.fields["InodeTableStart"] + (inode_no // INODES_PER_BLOCK)

    def _InodeTableOffset(self, inode_no):
        return (inode_no % INODES_PER_BLOCK) * INODE_SIZE

    def ReadInode(self, inode_no):
        if inode_no <= 0 or inode_no >= self.Super.fields["TotalInodes"]:
            raise Fs2Error("invalid inode number")
        block = bytearray(self.Image.ReadBlock(self._InodeTableBlock(inode_no)))
        offset = self._InodeTableOffset(inode_no)
        payload = bytes(block[offset:offset + INODE_SIZE])
        if len(payload) != INODE_SIZE:
            raise Fs2Error("inode record truncated")
        inode = Inode.unpack(payload)
        expected = Crc32(payload[:INODE_CHECKSUM_OFFSET] + b"\0\0\0\0" + payload[INODE_CHECKSUM_OFFSET + 4:])
        if inode.Checksum != expected:
            raise Fs2Error(f"inode {inode_no} checksum mismatch")
        return inode

    def WriteInode(self, inode):
        if inode.InodeNo <= 0 or inode.InodeNo >= self.Super.fields["TotalInodes"]:
            raise Fs2Error("invalid inode number")
        block_no = self._InodeTableBlock(inode.InodeNo)
        block = bytearray(self.Image.ReadBlock(block_no))
        offset = self._InodeTableOffset(inode.InodeNo)
        block[offset:offset + INODE_SIZE] = inode.pack()
        self.Image.WriteBlock(block_no, bytes(block))

    def _ReadIndirectTable(self, inode):
        if inode.IndirectBlock == 0:
            return []
        block = self.Image.ReadBlock(inode.IndirectBlock)
        entries = []
        for i in range(0, len(block), EXTENT_SIZE):
            chunk = block[i:i + EXTENT_SIZE]
            if len(chunk) != EXTENT_SIZE:
                break
            ext = Extent.unpack(chunk)
            if ext.Count == 0 and ext.DiskBlock == 0 and ext.FileBlock == 0:
                continue
            entries.append(ext)
        return entries

    def _WriteIndirectTable(self, inode, extents):
        if not extents:
            if inode.IndirectBlock:
                self.FreeBlocks([Extent(FileBlock=0, DiskBlock=inode.IndirectBlock, Count=1)])
                inode.IndirectBlock = 0
            return
        if inode.IndirectBlock == 0:
            inode.IndirectBlock = self.AllocBlocks(1)[0].DiskBlock
        payload = bytearray(BLOCK_SIZE)
        for i, ext in enumerate(extents[:170]):
            payload[i * EXTENT_SIZE:(i + 1) * EXTENT_SIZE] = ext.pack()
        self.Image.WriteBlock(inode.IndirectBlock, bytes(payload))

    def MapFileBlock(self, inode, fileBlock):
        if fileBlock is None:
            return None
        for ext in inode.Extents[:6]:
            if ext.Count == 0:
                continue
            end = ext.FileBlock + ext.Count
            if ext.FileBlock <= fileBlock < end:
                return ext.DiskBlock + (fileBlock - ext.FileBlock)
        for ext in self._ReadIndirectTable(inode):
            end = ext.FileBlock + ext.Count
            if ext.FileBlock <= fileBlock < end:
                return ext.DiskBlock + (fileBlock - ext.FileBlock)
        return None

    def AppendExtent(self, inode, disk_block, count, file_block=None):
        if file_block is None:
            file_block = inode.Size // BLOCK_SIZE if inode.Size else 0
        new_extent = Extent(FileBlock=file_block, DiskBlock=disk_block, Count=count)

        inline = inode.Extents[:6]
        last = None
        for ext in reversed(inline):
            if ext.Count != 0:
                last = ext
                break

        if last is not None and last.FileBlock + last.Count == file_block and last.DiskBlock + last.Count == disk_block:
            last.Count += count
            inode.ExtentCount = sum(1 for ext in inline if ext.Count)
            return

        for i, ext in enumerate(inline):
            if ext.Count == 0:
                inline[i] = new_extent
                inode.Extents = inline
                inode.ExtentCount = sum(1 for e in inline if e.Count)
                return

        indirect = self._ReadIndirectTable(inode)
        if indirect and indirect[-1].FileBlock + indirect[-1].Count == file_block and indirect[-1].DiskBlock + indirect[-1].Count == disk_block:
            indirect[-1].Count += count
        else:
            if len(indirect) >= 170:
                raise Fs2Error("inode indirect extent table is full")
            indirect.append(new_extent)
        inode.ExtentCount = sum(1 for ext in inline if ext.Count) + len(indirect)
        self._WriteIndirectTable(inode, indirect)

    def ReadData(self, inode, offset, length):
        if offset < 0 or length < 0:
            raise Fs2Error("offset and length must be non-negative")
        if offset >= inode.Size:
            return b""
        end = min(inode.Size, offset + length)
        out = bytearray()
        pos = offset
        while pos < end:
            block_index = pos // BLOCK_SIZE
            block_offset = pos % BLOCK_SIZE
            disk_block = self.MapFileBlock(inode, block_index)
            if disk_block is None:
                chunk = min(BLOCK_SIZE - block_offset, end - pos)
                out.extend(b"\0" * chunk)
                pos += chunk
                continue
            block = bytearray(self.Image.ReadBlock(disk_block))
            chunk = min(BLOCK_SIZE - block_offset, end - pos)
            out.extend(block[block_offset:block_offset + chunk])
            pos += chunk
        return bytes(out)

    def WriteData(self, inode, offset, data):
        if offset < 0:
            raise Fs2Error("offset must not be negative")
        if not data:
            return inode
        inode.Size = max(inode.Size, offset + len(data))
        pos = offset
        index = 0
        while index < len(data):
            block_index = pos // BLOCK_SIZE
            block_offset = pos % BLOCK_SIZE
            disk_block = self.MapFileBlock(inode, block_index)
            if disk_block is None:
                allocs = self.AllocBlocks(1)
                self.AppendExtent(inode, allocs[0].DiskBlock, 1, file_block=block_index)
                inode.Blocks += 1
                disk_block = allocs[0].DiskBlock
            block = bytearray(self.Image.ReadBlock(disk_block))
            chunk = min(BLOCK_SIZE - block_offset, len(data) - index)
            block[block_offset:block_offset + chunk] = data[index:index + chunk]
            self.Image.WriteBlock(disk_block, bytes(block))
            pos += chunk
            index += chunk
        inode.Mtime = int(time.time())
        inode.Ctime = inode.Mtime
        self.WriteInode(inode)
        return inode

    def Truncate(self, inode, new_size):
        if new_size < 0:
            raise Fs2Error("truncate size must not be negative")
        if new_size >= inode.Size:
            inode.Size = new_size
            self.WriteInode(inode)
            return inode
        keep = (new_size + BLOCK_SIZE - 1) // BLOCK_SIZE
        to_free = []
        for ext in inode.Extents[:6]:
            if ext.FileBlock >= keep:
                to_free.append(ext)
        if inode.IndirectBlock:
            for ext in self._ReadIndirectTable(inode):
                if ext.FileBlock >= keep:
                    to_free.append(ext)
        if to_free:
            self.FreeBlocks(to_free)
        inode.Extents = [ext for ext in inode.Extents if ext.FileBlock < keep]
        if inode.IndirectBlock:
            remaining = [ext for ext in self._ReadIndirectTable(inode) if ext.FileBlock < keep]
            if remaining:
                self._WriteIndirectTable(inode, remaining)
            else:
                self.FreeBlocks([Extent(FileBlock=0, DiskBlock=inode.IndirectBlock, Count=1)])
                inode.IndirectBlock = 0
        inode.Size = new_size
        inode.ExtentCount = len(inode.Extents)
        self.WriteInode(inode)
        return inode

    def FreeInodeData(self, inode):
        extents = list(inode.Extents)
        if inode.IndirectBlock:
            extents.extend(self._ReadIndirectTable(inode))
        if extents:
            self.FreeBlocks(extents)
        inode.Extents = []
        inode.ExtentCount = 0
        if inode.IndirectBlock:
            self.FreeBlocks([Extent(FileBlock=0, DiskBlock=inode.IndirectBlock, Count=1)])
            inode.IndirectBlock = 0
        inode.Size = 0
        inode.Blocks = 0
        self.WriteInode(inode)

    def DirLookup(self, dir_inode_no, name):
        inode = self.ReadInode(dir_inode_no)
        if inode.Mode >> 12 != INODE_TYPE_DIR:
            raise Fs2Error(f"inode {dir_inode_no} is not a directory")
        data = self.ReadData(inode, 0, inode.Size)
        pos = 0
        while pos + DIR_ENTRY_HEADER_SIZE <= len(data):
            inode_no, rec_len, name_len, _, _ = DIR_ENTRY_STRUCT.unpack_from(data, pos)
            if rec_len == 0:
                pos = min(len(data), ((pos // BLOCK_SIZE) + 1) * BLOCK_SIZE)
                continue
            if pos + rec_len > len(data):
                break
            entry_name = data[pos + DIR_ENTRY_HEADER_SIZE:pos + DIR_ENTRY_HEADER_SIZE + name_len]
            if inode_no and entry_name == name.encode("utf-8"):
                return inode_no
            pos += rec_len
        return 0

    def _ExtTag(self, name):
        ext = Path(name).suffix.lower().lstrip(".")
        if len(ext) > 7:
            ext = ext[:7]
        return ext.encode("ascii", "ignore")

    def _MakeInode(self, name, mode, inode_no=None):
        inode_no = inode_no if inode_no is not None else self.AllocInode()
        inode = Inode(
            Mode=mode,
            Links=1,
            Size=0,
            Blocks=0,
            Atime=int(time.time()),
            Mtime=int(time.time()),
            Ctime=int(time.time()),
            Crtime=int(time.time()),
            ExtentCount=0,
            InodeNo=inode_no,
            ExtTag=self._ExtTag(name),
        )
        self.WriteInode(inode)
        return inode_no, inode

    def DirAdd(self, dir_inode_no, name, file_type, inode_no=None):
        if not name or name in ('.', '..'):
            raise Fs2Error("invalid directory entry name")
        directory = self.ReadInode(dir_inode_no)
        if directory.Mode >> 12 != INODE_TYPE_DIR:
            raise Fs2Error(f"inode {dir_inode_no} is not a directory")
        if self.DirLookup(dir_inode_no, name):
            raise Fs2Error(f"entry '{name}' already exists")
        record = DirEntry(inode_no or 0, name, file_type).pack()
        data = bytearray(self.ReadData(directory, 0, directory.Size))
        if not data:
            data = bytearray(BLOCK_SIZE)

        slot = None
        slot_len = 0
        split_pos = None
        split_len = 0
        new_block = False
        pos = 0
        while pos + DIR_ENTRY_HEADER_SIZE <= len(data):
            inode_no_field, rec_len, name_len, _, _ = DIR_ENTRY_STRUCT.unpack_from(data, pos)
            if rec_len == 0:
                pos = min(len(data), ((pos // BLOCK_SIZE) + 1) * BLOCK_SIZE)
                continue
            if inode_no_field == 0 and rec_len >= len(record):
                slot = pos
                slot_len = rec_len
                break
            # Occupied records can have padding up to the end of a block.
            # Split that padding rather than allocating a block per entry.
            used_len = (DIR_ENTRY_HEADER_SIZE + name_len + 7) & ~7
            if inode_no_field and rec_len - used_len >= len(record):
                split_pos = pos
                split_len = used_len
                slot = pos + used_len
                slot_len = rec_len - used_len
                break
            pos += rec_len

        if slot is None:
            if directory.Size == 0:
                directory.Size = BLOCK_SIZE
            else:
                directory.Size += BLOCK_SIZE
            block = self.AllocBlocks(1)[0].DiskBlock
            self.AppendExtent(directory, block, 1, file_block=(directory.Size - BLOCK_SIZE) // BLOCK_SIZE)
            directory.Blocks += 1
            slot = directory.Size - BLOCK_SIZE
            slot_len = BLOCK_SIZE
            new_block = True

        block_index = slot // BLOCK_SIZE
        block_offset = slot % BLOCK_SIZE
        if block_offset + len(record) > BLOCK_SIZE:
            raise Fs2Error("directory entry does not fit in block")

        if new_block:
            # A reused disk block may contain stale directory entries.
            block_data = bytearray(BLOCK_SIZE)
        else:
            block_data = bytearray(self.ReadData(directory, block_index * BLOCK_SIZE, BLOCK_SIZE))
        if split_pos is not None:
            struct.pack_into("<H", block_data, split_pos % BLOCK_SIZE + 8, split_len)
        record = DirEntry(inode_no or 0, name, file_type).pack(rec_len=slot_len)
        block_data[block_offset:block_offset + len(record)] = record
        if inode_no and file_type == DIRENT_TYPE_DIR:
            # The new child's '..' adds a reference to this parent.
            directory.Links += 1
        self.WriteData(directory, block_index * BLOCK_SIZE, bytes(block_data))
        self.WriteInode(directory)
        return 0

    def DirRemove(self, dir_inode_no, name):
        if not name or name in ('.', '..'):
            raise Fs2Error("invalid directory entry name")
        directory = self.ReadInode(dir_inode_no)
        data = bytearray(self.ReadData(directory, 0, directory.Size))
        pos = 0
        while pos + DIR_ENTRY_HEADER_SIZE <= len(data):
            inode_no_field, rec_len, name_len, file_type, _ = DIR_ENTRY_STRUCT.unpack_from(data, pos)
            if rec_len == 0:
                pos = min(len(data), ((pos // BLOCK_SIZE) + 1) * BLOCK_SIZE)
                continue
            entry_name = data[pos + DIR_ENTRY_HEADER_SIZE:pos + DIR_ENTRY_HEADER_SIZE + name_len]
            if inode_no_field and entry_name == name.encode("utf-8"):
                # Keep the record length: merging across block boundaries
                # would corrupt later directory entries. DirAdd reuses it.
                struct.pack_into("<Q", data, pos, 0)
                if file_type == DIRENT_TYPE_DIR:
                    directory.Links -= 1
                break
            pos += rec_len
        self.WriteData(directory, 0, bytes(data))
        self.WriteInode(directory)
        return 0

    def DirList(self, dir_inode_no):
        inode = self.ReadInode(dir_inode_no)
        if inode.Mode >> 12 != INODE_TYPE_DIR:
            raise Fs2Error(f"inode {dir_inode_no} is not a directory")
        data = self.ReadData(inode, 0, inode.Size)
        entries = []
        pos = 0
        while pos + DIR_ENTRY_HEADER_SIZE <= len(data):
            inode_no_field, rec_len, name_len, file_type, _ = DIR_ENTRY_STRUCT.unpack_from(data, pos)
            if rec_len == 0:
                pos = min(len(data), ((pos // BLOCK_SIZE) + 1) * BLOCK_SIZE)
                continue
            if pos + rec_len > len(data):
                break
            if inode_no_field == 0:
                pos += rec_len
                continue
            name_bytes = data[pos + DIR_ENTRY_HEADER_SIZE:pos + DIR_ENTRY_HEADER_SIZE + name_len]
            entries.append({"inode": inode_no_field, "name": name_bytes.decode("utf-8", "replace"), "type": file_type})
            pos += rec_len
        return entries

    def ResolvePath(self, path):
        if path in ("", "/"):
            return [1]
        parts = [p for p in path.split("/") if p]
        current = 1
        chain = [current]
        for part in parts:
            next_inode = self.DirLookup(current, part)
            if not next_inode:
                raise Fs2Error(f"path not found: {path}")
            current = next_inode
            chain.append(current)
        return chain

    def Flush(self):
        self.Super.fields["FreeBlocks"] = self.FreeBlocksCount
        self.Super.fields["FreeInodes"] = self.FreeInodesCount
        self.Super.fields["WrittenTime"] = int(time.time())
        self._WriteBitmap(self.Super.fields["BlockBitmapStart"], self.BlockBitmap)
        self._WriteBitmap(self.Super.fields["InodeBitmapStart"], self.InodeBitmap)
        packed = Superblock(**self.Super.fields).pack()
        self.Image.WriteBlock(0, packed)
        self.Image.WriteBlock(self.Super.fields["BackupBlock"], packed)


def _CreateImage(path, size, force):
    path = Path(path)
    if path.exists() and not force:
        raise Fs2Error(f"image already exists: {path} (use --force to replace it)")
    if size % SECTOR_SIZE:
        raise Fs2Error("image size must be a multiple of 512 bytes")
    mode = "wb" if force else "xb"
    try:
        with path.open(mode) as f:
            f.truncate(size)
    except FileExistsError:
        raise Fs2Error(f"image already exists: {path} (use --force to replace it)")
    print(f"Created sparse image: {path} ({size} bytes)")


def _FormatImage(path, label, offset, size, bytes_per_inode):
    path = Path(path)
    if not path.is_file():
        raise Fs2Error(f"image does not exist: {path}")
    image_sectors = path.stat().st_size // SECTOR_SIZE
    if offset >= image_sectors:
        raise Fs2Error("offset leaves no space for a filesystem")
    if size is not None and size % SECTOR_SIZE:
        raise Fs2Error("filesystem size must be a multiple of 512 bytes")
    if size is None:
        total_sectors = image_sectors
    else:
        total_sectors = offset + (size // SECTOR_SIZE)
        if (size // SECTOR_SIZE) > (image_sectors - offset):
            raise Fs2Error("filesystem would exceed image bounds")

    geometry = CalcGeometry(total_sectors, offset, bytes_per_inode)
    label_bytes = label.encode("utf-8")
    if b"\0" in label_bytes or len(label_bytes) > 63:
        raise Fs2Error("label must be NUL-free and at most 63 UTF-8 bytes")

    now = int(time.time())
    superblock = Superblock(
        Magic=SUPERBLOCK_MAGIC,
        VersionMajor=1,
        VersionMinor=0,
        BlockSize=BLOCK_SIZE,
        TotalBlocks=geometry["TotalBlocks"],
        FreeBlocks=geometry["FreeBlocks"],
        TotalInodes=geometry["TotalInodes"],
        FreeInodes=geometry["FreeInodes"],
        InodeSize=INODE_SIZE,
        Flags=0,
        BlockBitmapStart=geometry["BlockBitmapStart"],
        BlockBitmapBlocks=geometry["BlockBitmapBlocks"],
        InodeBitmapStart=geometry["InodeBitmapStart"],
        InodeBitmapBlocks=geometry["InodeBitmapBlocks"],
        InodeTableStart=geometry["InodeTableStart"],
        InodeTableBlocks=geometry["InodeTableBlocks"],
        DataStart=geometry["DataStart"],
        RootInode=1,
        BackupBlock=geometry["BackupBlock"],
        BootFileBlock=0,
        BootFileBlocks=0,
        BootFileSize=0,
        Uuid=uuid.uuid4().bytes,
        Label=label_bytes,
        CreatedTime=now,
        MountedTime=0,
        WrittenTime=now,
        MountCount=0,
    )
    superblock_bytes = superblock.pack()

    block_bitmap = bytearray(geometry["BlockBitmapBlocks"] * BLOCK_SIZE)
    _set_bitmap_range(block_bitmap, 0, geometry["DataStart"] + 1)
    _set_bitmap_range(block_bitmap, geometry["BackupBlock"], 1)
    inode_bitmap = bytearray(geometry["InodeBitmapBlocks"] * BLOCK_SIZE)
    _set_bitmap_range(inode_bitmap, 0, 2)

    root_inode = Inode(
        Mode=(INODE_TYPE_DIR << 12) | 0o755,
        Links=2,
        Size=BLOCK_SIZE,
        Blocks=1,
        Atime=now,
        Mtime=now,
        Ctime=now,
        Crtime=now,
        ExtentCount=1,
        InodeNo=1,
        Extents=[Extent(FileBlock=0, DiskBlock=geometry["DataStart"], Count=1)],
    )

    dot = DirEntry(1, ".", DIRENT_TYPE_DIR).pack(rec_len=24)
    dotdot = DirEntry(1, "..", DIRENT_TYPE_DIR).pack(rec_len=BLOCK_SIZE - len(dot))
    root_dir_data = dot + dotdot

    with Image(path, offset, geometry["TotalBlocks"]) as img:
        img.ZeroBlocks(1, geometry["DataStart"] - 1)
        img.WriteBlocks(geometry["BlockBitmapStart"], block_bitmap)
        img.WriteBlocks(geometry["InodeBitmapStart"], inode_bitmap)
        inode_table = bytearray(BLOCK_SIZE)
        inode_table[INODE_SIZE:INODE_SIZE * 2] = root_inode.pack()
        img.WriteBlock(geometry["InodeTableStart"], inode_table)
        if geometry["InodeTableBlocks"] > 1:
            for i in range(1, geometry["InodeTableBlocks"]):
                img.WriteBlock(geometry["InodeTableStart"] + i, b"\0" * BLOCK_SIZE)
        img.WriteBlock(geometry["DataStart"], root_dir_data)
        img.WriteBlock(0, superblock_bytes)
        img.WriteBlock(geometry["BackupBlock"], superblock_bytes)
        img.File.flush(); os.fsync(img.File.fileno())

    print(f"Formatted {path} (label {label!r})")
    print(f"  Blocks: {geometry['TotalBlocks']} total, {geometry['FreeBlocks']} free")
    print(f"  Inodes: {geometry['TotalInodes']} total, {geometry['FreeInodes']} free")
    print(f"  Block bitmap start: {geometry['BlockBitmapStart']}")
    print(f"  Inode bitmap start: {geometry['InodeBitmapStart']}")
    print(f"  Inode table start: {geometry['InodeTableStart']}")
    print(f"  Data start: {geometry['DataStart']}")
    print(f"  Backup superblock: {geometry['BackupBlock']}")


def _print_info(volume):
    sb = volume.Super.fields
    label = sb["Label"].split(b"\0", 1)[0].decode("utf-8", "replace")
    print(f"Image: {volume.Image.Path}")
    print(f"Label: {label}")
    print(f"Magic: 0x{sb['Magic']:016x}")
    print(f"Version: {sb['VersionMajor']}.{sb['VersionMinor']}")
    print(f"Blocks: {sb['TotalBlocks']} total, {sb['FreeBlocks']} free")
    print(f"Inodes: {sb['TotalInodes']} total, {sb['FreeInodes']} free")
    print(f"Block bitmap: {sb['BlockBitmapStart']} ({sb['BlockBitmapBlocks']} blocks)")
    print(f"Inode bitmap: {sb['InodeBitmapStart']} ({sb['InodeBitmapBlocks']} blocks)")
    print(f"Inode table: {sb['InodeTableStart']} ({sb['InodeTableBlocks']} blocks)")
    print(f"Data start: {sb['DataStart']}")
    print(f"Backup superblock: {sb['BackupBlock']}")


def _split_parent_child(path):
    if path in ("", "/"):
        raise Fs2Error("empty path is invalid")
    clean = path.strip("/")
    if not clean:
        raise Fs2Error("empty path is invalid")
    parent = str(Path(clean).parent)
    if parent in (".", ""):
        parent = "/"
    elif not parent.startswith("/"):
        parent = "/" + parent
    name = Path(clean).name
    return (parent, name)


def _file_flags_to_string(flags):
    out = []
    if flags & INODE_FLAG_EXEC:
        out.append("exec")
    if flags & INODE_FLAG_CONTIG:
        out.append("contig")
    return ",".join(out) if out else "none"


def _stat_volume_path(volume, path):
    inode_no = volume.ResolvePath(path)[-1]
    inode = volume.ReadInode(inode_no)
    ext_count = sum(1 for ext in inode.Extents if ext.Count)
    if inode.IndirectBlock:
        ext_count += len(volume._ReadIndirectTable(inode))
    return inode_no, inode, ext_count


def _remove_tree(volume, inode_no):
    inode = volume.ReadInode(inode_no)
    if inode.Mode >> 12 == INODE_TYPE_DIR:
        for entry in volume.DirList(inode_no):
            if entry["name"] in (".", ".."):
                continue
            _remove_tree(volume, entry["inode"])
    volume.FreeInodeData(inode)
    volume.FreeInode(inode_no)


def _mkdir_recursive(volume, path):
    if path in ("", "/"):
        raise Fs2Error("cannot mkdir '/'")
    parts = [p for p in path.split("/") if p]
    current = 1
    for name in parts:
        if name in ('.', '..'):
            continue
        existing = volume.DirLookup(current, name)
        if existing:
            current = existing
            continue
        inode_no = volume.AllocInode()
        now = int(time.time())
        block = volume.AllocBlocks(1)[0].DiskBlock
        inode = Inode(
            Mode=(INODE_TYPE_DIR << 12) | 0o755,
            Links=2,
            Size=BLOCK_SIZE,
            Blocks=1,
            Atime=now,
            Mtime=now,
            Ctime=now,
            Crtime=now,
            ExtentCount=1,
            InodeNo=inode_no,
            ExtTag="",
            Extents=[Extent(FileBlock=0, DiskBlock=block, Count=1)],
        )
        volume.WriteInode(inode)
        dot = DirEntry(inode_no, ".", DIRENT_TYPE_DIR).pack(rec_len=24)
        dotdot = DirEntry(current, "..", DIRENT_TYPE_DIR).pack(rec_len=BLOCK_SIZE - len(dot))
        volume.Image.WriteBlock(block, dot + dotdot)
        volume.DirAdd(current, name, DIRENT_TYPE_DIR, inode_no)
        current = inode_no
    return current


def _collect_extent_list(volume, inode):
    extents = []
    for ext in inode.Extents:
        if ext.Count:
            extents.append(ext)
    if inode.IndirectBlock:
        extents.extend(volume._ReadIndirectTable(inode))
    return extents


def _check_volume(volume):
    errors = []
    total_blocks = volume.Super.fields["TotalBlocks"]
    total_inodes = volume.Super.fields["TotalInodes"]

    def add_error(msg):
        errors.append(msg)

    primary = volume.Image.ReadBlock(0)
    sb = Superblock.unpack(primary)
    if sb.fields.get("Magic") != SUPERBLOCK_MAGIC:
        add_error("superblock magic mismatch")
    if sb.fields.get("Checksum") != Crc32(primary[:SUPERBLOCK_CHECKSUM_OFFSET]):
        add_error("superblock checksum mismatch")

    backup_block = sb.fields.get("BackupBlock", 0)
    if 0 <= backup_block < total_blocks:
        backup = volume.Image.ReadBlock(backup_block)
        backup_sb = Superblock.unpack(backup)
        if backup_sb.fields.get("Magic") != SUPERBLOCK_MAGIC:
            add_error("backup superblock magic mismatch")
        if backup_sb.fields.get("Checksum") != Crc32(backup[:SUPERBLOCK_CHECKSUM_OFFSET]):
            add_error("backup superblock checksum mismatch")
        for key in ("TotalBlocks", "FreeBlocks", "TotalInodes", "FreeInodes", "DataStart", "BackupBlock"):
            if sb.fields.get(key) != backup_sb.fields.get(key):
                add_error(f"primary and backup superblock differ in {key}")
    else:
        add_error("invalid backup block")

    expected_free_blocks = sum(1 for i in range(total_blocks) if not volume._TestBit(volume.BlockBitmap, i))
    if volume.FreeBlocksCount != expected_free_blocks:
        add_error(f"free block count mismatch: volume={volume.FreeBlocksCount}, bitmap={expected_free_blocks}")

    expected_free_inodes = sum(1 for i in range(total_inodes) if not volume._TestBit(volume.InodeBitmap, i))
    if volume.FreeInodesCount != expected_free_inodes:
        add_error(f"free inode count mismatch: volume={volume.FreeInodesCount}, bitmap={expected_free_inodes}")

    seen_inodes = set()
    seeks = {}

    def walk(inode_no):
        if inode_no in seen_inodes:
            add_error(f"inode {inode_no} appears in a cycle")
            return
        seen_inodes.add(inode_no)
        try:
            inode = volume.ReadInode(inode_no)
        except Fs2Error as exc:
            add_error(f"inode {inode_no}: {exc}")
            return

        if inode.InodeNo != inode_no:
            add_error(f"inode number mismatch at slot {inode_no}: stored as {inode.InodeNo}")

        extents = _collect_extent_list(volume, inode)
        used_blocks = set()
        for ext in extents:
            if ext.Count <= 0:
                continue
            start = int(ext.DiskBlock)
            stop = start + int(ext.Count)
            if start < 0 or stop > total_blocks:
                add_error(f"extent for inode {inode_no} exceeds volume bounds: {start}:{stop}")
                continue
            for block in range(start, stop):
                if block in used_blocks:
                    add_error(f"inode {inode_no} reuses block {block} in extents")
                used_blocks.add(block)
                if not volume._TestBit(volume.BlockBitmap, block):
                    add_error(f"block {block} used by inode {inode_no} is not set in block bitmap")

        if inode.Mode >> 12 == INODE_TYPE_DIR:
            for entry in volume.DirList(inode_no):
                child = entry["inode"]
                # '.' and '..' are real links, but must not be traversed.
                seeks[child] = seeks.get(child, 0) + 1
                if entry["name"] in (".", ".."):
                    continue
                walk(child)

    walk(1)

    for inode_no in range(1, total_inodes):
        if not volume._TestBit(volume.InodeBitmap, inode_no):
            continue
        try:
            inode = volume.ReadInode(inode_no)
        except Fs2Error as exc:
            add_error(f"inode {inode_no}: {exc}")
            continue
        if inode.InodeNo != inode_no:
            add_error(f"inode number mismatch at slot {inode_no}: stored as {inode.InodeNo}")
        expected_links = seeks.get(inode_no, 0)
        if inode.Links != expected_links:
            add_error(f"inode {inode_no} link count mismatch: stored={inode.Links}, discovered={expected_links}")

    for inode_no in range(1, total_inodes):
        if not volume._TestBit(volume.InodeBitmap, inode_no):
            continue
        try:
            volume.ReadInode(inode_no)
        except Fs2Error as exc:
            add_error(f"inode {inode_no}: {exc}")
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description="Create and manage 2fs images.")
    sub = parser.add_subparsers(dest="command", required=True)

    create = sub.add_parser("create", help="create a sparse image")
    create.add_argument("img")
    create.add_argument("--size", required=True, type=ParseSize)
    create.add_argument("--force", action="store_true")

    fmt = sub.add_parser("format", help="format a 2fs volume")
    fmt.add_argument("img")
    fmt.add_argument("--label", required=True)
    fmt.add_argument("--offset", type=int, default=2048)
    fmt.add_argument("--size", type=ParseSize, default=None)
    fmt.add_argument("--bytes-per-inode", type=int, default=16384)

    info = sub.add_parser("info", help="show volume metadata")
    info.add_argument("img")

    ls = sub.add_parser("ls", help="list a directory")
    ls.add_argument("img")
    ls.add_argument("path", nargs="?", default="/")
    ls.add_argument("-l", action="store_true")

    mkdir = sub.add_parser("mkdir", help="create a directory")
    mkdir.add_argument("img")
    mkdir.add_argument("path")
    mkdir.add_argument("-p", action="store_true")

    bootcode = sub.add_parser("bootcode", help="install MBR and stage2 boot code")
    bootcode.add_argument("img")
    bootcode.add_argument("--mbr", required=True)
    bootcode.add_argument("--stage2", required=True)

    setboot = sub.add_parser("setboot", help="mark a contiguous file as the boot file")
    setboot.add_argument("img")
    setboot.add_argument("path")

    check = sub.add_parser("check", help="verify volume metadata and filesystem integrity")
    check.add_argument("img")

    put = sub.add_parser("put", help="create or replace a regular file")
    put.add_argument("img")
    put.add_argument("local_path")
    put.add_argument("dest_path")
    put.add_argument("--contiguous", action="store_true")
    put.add_argument("--exec", action="store_true")

    get = sub.add_parser("get", help="copy a file from the image to local disk")
    get.add_argument("img")
    get.add_argument("src_path")
    get.add_argument("local_path")

    cat = sub.add_parser("cat", help="print a file to stdout")
    cat.add_argument("img")
    cat.add_argument("path")

    rm = sub.add_parser("rm", help="remove a file or directory")
    rm.add_argument("img")
    rm.add_argument("path")
    rm.add_argument("-r", action="store_true")

    mv = sub.add_parser("mv", help="move or rename a file or directory")
    mv.add_argument("img")
    mv.add_argument("src_path")
    mv.add_argument("dest_path")

    stat = sub.add_parser("stat", help="show inode metadata")
    stat.add_argument("img")
    stat.add_argument("path")

    args = parser.parse_args(argv)
    try:
        if args.command == "create":
            _CreateImage(args.img, args.size, args.force)
        elif args.command == "format":
            if args.offset < 0:
                raise Fs2Error("offset must not be negative")
            _FormatImage(args.img, args.label, args.offset, args.size, args.bytes_per_inode)
        elif args.command == "info":
            volume = Volume(args.img)
            _print_info(volume)
        elif args.command == "ls":
            volume = Volume(args.img)
            inode_no = volume.ResolvePath(args.path)[-1]
            for entry in volume.DirList(inode_no):
                if args.l:
                    inode = volume.ReadInode(entry["inode"])
                    kind = "dir" if inode.Mode >> 12 == INODE_TYPE_DIR else "file"
                    print(f"{entry['inode']:>6} {kind:<4} {inode.Size:<8} {entry['name']}")
                else:
                    print(entry["name"])
        elif args.command == "mkdir":
            volume = Volume(args.img)
            if args.p:
                _mkdir_recursive(volume, args.path)
            else:
                path = args.path.strip("/")
                if not path:
                    raise Fs2Error("refusing to create root directory")
                parent, name = _split_parent_child(path)
                parent_inode = volume.ResolvePath(parent)[-1]
                if volume.DirLookup(parent_inode, name):
                    raise Fs2Error(f"entry '{name}' already exists")
                inode_no = volume.AllocInode()
                now = int(time.time())
                block = volume.AllocBlocks(1)[0].DiskBlock
                inode = Inode(
                    Mode=(INODE_TYPE_DIR << 12) | 0o755,
                    Links=2,
                    Size=BLOCK_SIZE,
                    Blocks=1,
                    Atime=now,
                    Mtime=now,
                    Ctime=now,
                    Crtime=now,
                    ExtentCount=1,
                    InodeNo=inode_no,
                    ExtTag=b"",
                    Extents=[Extent(FileBlock=0, DiskBlock=block, Count=1)],
                )
                volume.WriteInode(inode)
                dot = DirEntry(inode_no, ".", DIRENT_TYPE_DIR).pack(rec_len=24)
                dotdot = DirEntry(parent_inode, "..", DIRENT_TYPE_DIR).pack(rec_len=BLOCK_SIZE - len(dot))
                volume.Image.WriteBlock(block, dot + dotdot)
                volume.DirAdd(parent_inode, name, DIRENT_TYPE_DIR, inode_no)
            volume.Flush()
            print(f"mkdir: {args.path}")
        elif args.command == "bootcode":
            volume = Volume(args.img)
            if volume.OffsetSectors < 33:
                raise Fs2Error("volume offset must be at least 33 sectors")
            with open(args.mbr, "rb") as fh:
                mbr = fh.read()
            if len(mbr) != 512:
                raise Fs2Error("MBR must be exactly 512 bytes")
            if mbr[-2:] != b"\x55\xaa":
                raise Fs2Error("MBR must end with 0x55 0xAA")
            with open(args.stage2, "rb") as fh:
                stage2 = fh.read()
            if len(stage2) > 32 * 512:
                raise Fs2Error("stage2 payload exceeds 32 * 512 bytes")
            padded_stage = stage2 + (b"\0" * (32 * 512 - len(stage2)))
            with open(args.img, "r+b") as fh:
                fh.seek(0)
                fh.write(mbr)
                fh.seek(512)
                fh.write(padded_stage)
        elif args.command == "setboot":
            volume = Volume(args.img)
            inode_no = volume.ResolvePath(args.path)[-1]
            inode = volume.ReadInode(inode_no)
            if inode.Mode >> 12 == INODE_TYPE_DIR:
                raise Fs2Error("boot file must be a regular file")
            extents = _collect_extent_list(volume, inode)
            if len(extents) != 1:
                raise Fs2Error("boot file must have exactly one extent")
            extent = extents[0]
            if not (inode.Flags & INODE_FLAG_CONTIG):
                raise Fs2Error("boot file must have the Contiguous flag set")
            volume.Super.fields["BootFileBlock"] = extent.DiskBlock
            volume.Super.fields["BootFileBlocks"] = extent.Count
            volume.Super.fields["BootFileSize"] = inode.Size
            volume.Flush()
        elif args.command == "check":
            volume = Volume(args.img)
            errors = _check_volume(volume)
            if errors:
                for msg in errors:
                    print(f"ERROR: {msg}", file=sys.stderr)
                return 1
            print("OK")
            return 0
        elif args.command == "put":
            with open(args.local_path, "rb") as fh:
                data = fh.read()
            volume = Volume(args.img)
            dest = args.dest_path.strip("/")
            if not dest:
                raise Fs2Error("destination path cannot be empty")
            parent, name = _split_parent_child(dest)
            parent_inode = volume.ResolvePath(parent)[-1]
            inode_no = volume.DirLookup(parent_inode, name)
            to_reuse = None
            if inode_no:
                inode = volume.ReadInode(inode_no)
                if inode.Mode >> 12 == INODE_TYPE_DIR:
                    raise Fs2Error(f"refusing to overwrite directory '{dest}'")
                to_reuse = inode
            else:
                inode_no = volume.AllocInode()
                inode = Inode(
                    Mode=(INODE_TYPE_FILE << 12) | 0o644,
                    Links=1,
                    Size=0,
                    Blocks=0,
                    Atime=int(time.time()),
                    Mtime=int(time.time()),
                    Ctime=int(time.time()),
                    Crtime=int(time.time()),
                    InodeNo=inode_no,
                    ExtTag=volume._ExtTag(name),
                )
                volume.WriteInode(inode)
                volume.DirAdd(parent_inode, name, DIRENT_TYPE_FILE, inode_no)
            inode.InodeNo = inode_no
            inode.ExtTag = volume._ExtTag(name)
            if args.exec or name.lower().endswith(".exf"):
                inode.Flags |= INODE_FLAG_EXEC
            else:
                inode.Flags &= ~INODE_FLAG_EXEC
            if args.contiguous:
                blocks_needed = (len(data) + BLOCK_SIZE - 1) // BLOCK_SIZE
                if blocks_needed <= 0:
                    # Empty files reserve no data blocks.
                    volume.FreeInodeData(inode)
                    inode.Flags &= ~INODE_FLAG_CONTIG
                    inode.Size = 0
                    inode.Blocks = 0
                    inode.Mtime = int(time.time())
                    inode.Ctime = inode.Mtime
                    volume.WriteInode(inode)
                else:
                    # Reserve the contiguous run BEFORE releasing the old
                    # extents so a failed reservation cannot destroy data.
                    allocs = volume.AllocBlocks(blocks_needed, contiguous=True)
                    volume.FreeInodeData(inode)
                    inode.Flags |= INODE_FLAG_CONTIG
                    inode.Extents = [Extent(FileBlock=0, DiskBlock=allocs[0].DiskBlock, Count=blocks_needed)]
                    inode.IndirectBlock = 0
                    inode.ExtentCount = 1
                    inode.Blocks = blocks_needed
                    inode.Size = 0
                    volume.WriteInode(inode)
                    volume.WriteData(inode, 0, data)
            else:
                inode.Flags &= ~INODE_FLAG_CONTIG
                volume.FreeInodeData(inode)
                inode.Size = 0
                inode.Blocks = 0
                inode.ExtentCount = 0
                volume.WriteInode(inode)
                if len(data):
                    volume.WriteData(inode, 0, data)
                else:
                    inode.Mtime = int(time.time())
                    inode.Ctime = inode.Mtime
                    volume.WriteInode(inode)
            volume.Flush()
        elif args.command == "get":
            volume = Volume(args.img)
            inode_no = volume.ResolvePath(args.src_path)[-1]
            inode = volume.ReadInode(inode_no)
            if inode.Mode >> 12 == INODE_TYPE_DIR:
                raise Fs2Error(f"'{args.src_path}' is a directory")
            payload = volume.ReadData(inode, 0, inode.Size)
            with open(args.local_path, "wb") as fh:
                fh.write(payload)
        elif args.command == "cat":
            volume = Volume(args.img)
            inode_no = volume.ResolvePath(args.path)[-1]
            inode = volume.ReadInode(inode_no)
            if inode.Mode >> 12 == INODE_TYPE_DIR:
                raise Fs2Error(f"'{args.path}' is a directory")
            sys.stdout.buffer.write(volume.ReadData(inode, 0, inode.Size))
        elif args.command == "rm":
            volume = Volume(args.img)
            if args.path in ("", "/"):
                raise Fs2Error("refusing to remove the root directory")
            parent, name = _split_parent_child(args.path)
            parent_inode = volume.ResolvePath(parent)[-1]
            child = volume.DirLookup(parent_inode, name)
            if not child:
                raise Fs2Error(f"path not found: {args.path}")
            if child == 1:
                raise Fs2Error("refusing to remove the root directory")
            inode = volume.ReadInode(child)
            if inode.Mode >> 12 == INODE_TYPE_DIR:
                if not args.r:
                    # Root/empty directories list only "." and "..": anything
                    # else means the directory is not empty.
                    children = [e for e in volume.DirList(child) if e["name"] not in (".", "..")]
                    if children:
                        raise Fs2Error(f"directory '{args.path}' is not empty (use -r)")
                else:
                    for entry in list(volume.DirList(child)):
                        if entry["name"] not in (".", ".."):
                            _remove_tree(volume, entry["inode"])
                    volume.FreeInodeData(inode)
                    volume.FreeInode(child)
                    volume.DirRemove(parent_inode, name)
                    volume.Flush()
                    return 0
                # Non-recursive removal of an empty directory.
                volume.FreeInodeData(inode)
                volume.FreeInode(child)
            else:
                volume.FreeInodeData(inode)
                volume.FreeInode(child)
            volume.DirRemove(parent_inode, name)
            volume.Flush()
        elif args.command == "mv":
            volume = Volume(args.img)
            src = args.src_path.strip("/")
            dst = args.dest_path.strip("/")
            if not src or not dst:
                raise Fs2Error("source and destination paths cannot be empty")
            src_parent, src_name = _split_parent_child(src)
            dst_parent, dst_name = _split_parent_child(dst)
            src_chain = volume.ResolvePath("/" + src)
            src_parent_inode = volume.ResolvePath(src_parent)[-1]
            dst_parent_inode = volume.ResolvePath(dst_parent)[-1]
            inode_no = volume.DirLookup(src_parent_inode, src_name)
            if not inode_no:
                raise Fs2Error(f"path not found: {args.src_path}")
            if inode_no == 1 or src_chain[0] == 1 and len(src_chain) == 1:
                raise Fs2Error("refusing to move the root directory")
            if volume.DirLookup(dst_parent_inode, dst_name):
                raise Fs2Error(f"destination '{args.dest_path}' already exists")
            inode = volume.ReadInode(inode_no)
            is_dir = inode.Mode >> 12 == INODE_TYPE_DIR
            if is_dir:
                # Refuse to move a directory inside itself (would orphan it).
                dst_chain = volume.ResolvePath(dst_parent)
                if inode_no in dst_chain:
                    raise Fs2Error("cannot move a directory inside itself")
                if src_parent_inode == dst_parent_inode and src_name == dst_name:
                    raise Fs2Error("source and destination are the same")
            volume.DirAdd(dst_parent_inode, dst_name,
                          DIRENT_TYPE_DIR if is_dir else DIRENT_TYPE_FILE,
                          inode_no)
            volume.DirRemove(src_parent_inode, src_name)
            if is_dir and src_parent_inode != dst_parent_inode:
                # Re-point ".." at the new parent directory.
                raw = bytearray(volume.ReadData(inode, 0, inode.Size))
                pos = 0
                while pos + DIR_ENTRY_HEADER_SIZE <= len(raw):
                    _, rec_len, name_len, _, _ = DIR_ENTRY_STRUCT.unpack_from(raw, pos)
                    if rec_len == 0:
                        pos = min(len(raw), ((pos // BLOCK_SIZE) + 1) * BLOCK_SIZE)
                        continue
                    if pos + rec_len > len(raw):
                        break
                    name = bytes(raw[pos + DIR_ENTRY_HEADER_SIZE:pos + DIR_ENTRY_HEADER_SIZE + name_len])
                    if name == b"..":
                        struct.pack_into("<Q", raw, pos, dst_parent_inode)
                        break
                    pos += rec_len
                volume.WriteData(inode, 0, bytes(raw))
                volume.WriteInode(inode)
            volume.Flush()
        elif args.command == "stat":
            volume = Volume(args.img)
            inode_no, inode, ext_count = _stat_volume_path(volume, args.path)
            flags = _file_flags_to_string(inode.Flags)
            kind = "directory" if inode.Mode >> 12 == INODE_TYPE_DIR else "file"
            print(f"inode: {inode_no}")
            print(f"type: {kind}")
            print(f"size: {inode.Size}")
            print(f"extents: {ext_count}")
            print(f"flags: {inode.Flags} ({flags})")
            print(f"atime: {inode.Atime}")
            print(f"mtime: {inode.Mtime}")
            print(f"ctime: {inode.Ctime}")
            print(f"crtime: {inode.Crtime}")
    except (Fs2Error, OSError, OverflowError, struct.error, ValueError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
