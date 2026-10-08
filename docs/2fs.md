# 2fs v1.0: 64-bit, little endian, block size 4096

The volume starts at LBA 2048 of the disk (Fs2StartLba). Block numbers are volume-relative.
Sector = 512 bytes, 8 sectors per block. LBA = Fs2StartLba + Block * 8.

## Layout
Block 0              Superblock
Block 1              Reserved (zero)
Block 2..            Block bitmap  (BlockBitmapBlocks)
then                 Inode bitmap  (InodeBitmapBlocks)
then                 Inode table   (InodeTableBlocks)
then                 Data blocks
Last block           Backup superblock

One bit per block or inode, 1 = used. Everything before DataStart and the backup block is marked used.
Inode 0 is invalid (marked used). Inode 1 is the root directory.
TotalInodes = TotalBlocks / 4 rounded up to a multiple of 16. 16 inodes per block.
Bits per bitmap block: 32768.

## Superblock (block 0, 4096 bytes, rest zero)
off   size    field
0     u64     Magic            0x4E4552572D534632 (bytes "2FS-WREN")
8     u16     VersionMajor     1
10    u16     VersionMinor     0
12    u32     BlockSize        4096
16    u64     TotalBlocks
24    u64     FreeBlocks
32    u64     TotalInodes
40    u64     FreeInodes
48    u32     InodeSize        256
52    u32     Flags            bit0 Dirty (set while mounted), bit1 Error
56    u64     BlockBitmapStart
64    u64     BlockBitmapBlocks
72    u64     InodeBitmapStart
80    u64     InodeBitmapBlocks
88    u64     InodeTableStart
96    u64     InodeTableBlocks
104   u64     DataStart
112   u64     RootInode        1
120   u64     BackupBlock      last block of the volume
128   u64     BootFileBlock    first block of the boot file (contiguous), 0 = none
136   u64     BootFileBlocks
144   u64     BootFileSize     bytes
152   u8[16]  Uuid
168   char[64] Label           UTF-8, NUL terminated
232   u64     CreatedTime      seconds since 1970
240   u64     MountedTime
248   u64     WrittenTime
256   u32     MountCount
260   ...     Reserved (zero)
4092  u32     Checksum         CRC32 of bytes 0..4091

## Inode (256 bytes)
0     u16     Mode             bits 12-15 type (1 file, 2 dir, 3 symlink), bits 0-11 permissions
2     u16     Flags            bit0 Exec, bit1 System, bit2 Hidden, bit3 Immutable, bit4 Contiguous
4     u32     Uid
8     u32     Gid
12    u32     Links
16    u64     Size             bytes
24    u64     Blocks           allocated blocks (data + indirect)
32    u64     Atime
40    u64     Mtime
48    u64     Ctime
56    u64     Crtime
64    u32     ExtentCount      inline + indirect
68    u32     Reserved
72    u64     IndirectBlock    block with up to 170 more extents, 0 = none
80    char[8] ExtTag           lowercase extension without dot, NUL padded (empty for dirs)
88    u64     InodeNo
96    Extent[6]                inline extents (144 bytes)
240   u32     Checksum         CRC32 of the 256 bytes with this field as 0
244   u8[12]  Reserved

## Extent (24 bytes)
0     u64     FileBlock        first logical block
8     u64     DiskBlock        first block in the volume
16    u32     Count            blocks
20    u32     Flags            0

The first 6 extents are inline, the rest go in the indirect block (170 entries). Max 176 extents per file.

## Directory entry (variable length)
0     u64     InodeNo          0 = free slot
8     u16     RecLen           multiple of 8, covers the entry; the last entry of a block reaches the block end
10    u8      NameLen
11    u8      FileType         1 file, 2 dir, 3 symlink
12    u32     NameHash         FNV-1a 32 of the name
16    char[]  Name             NameLen bytes, no NUL

Entries never cross a block boundary. A directory size is a multiple of 4096.
The first two entries are "." and "..". Names: UTF-8, 1-255 bytes, no '/' and no NUL, case sensitive.
Inode number N uses bit N of the inode bitmap and slot N of the inode table.