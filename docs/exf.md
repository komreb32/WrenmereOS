# EXF v1: little endian, offsets are file offsets

## Header (64 bytes, offset 0)
off  size  field
0    u32   Magic          0x00465845 ("EXF\0")
4    u16   Version        1
6    u16   HeaderSize     64
8    u8    Kind           1 kernel, 2 program, 3 driver, 4 library
9    u8    Arch           1 x86_64
10   u16   Flags          0 (reserved)
12   u16   SegmentCount   max 16
14   u16   SegmentSize    40
16   u64   Entry          virtual address
24   u32   SegmentOffset  64
28   u32   SymbolCount
32   u32   SymbolOffset
36   u32   StringOffset
40   u32   StringSize
44   u32   FileSize
48   u32   Checksum       CRC32 (IEEE, poly 0xEDB88320) of the whole file, this field as 0
52   u8[12] Reserved      0

## Segment (40 bytes)
0    u32   Type           1 = Load
4    u32   Flags          bit0 Read, bit1 Write, bit2 Execute
8    u64   FileOffset     aligned to 16
16   u64   VirtAddr
24   u64   PhysAddr       used only when Kind = kernel, else 0
32   u32   FileSize
36   u32   MemSize        >= FileSize, the rest is zeroed on load

## Symbol (16 bytes), sorted by Value
0    u64   Value          virtual address
8    u32   Size
12   u32   NameOffset     offset inside the string table, NUL terminated

## Order in the file
Header, segment table, segment data, symbol table, string table.