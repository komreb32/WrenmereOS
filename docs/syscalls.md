# Syscalls (names and numbers are final). Unknown number -> ErrUnsupported.

## Processes
1  ExitProcess(Code)
2  SpawnProcess(PathPtr, ArgsPtr, ArgsLen, ArgCount, Caps) -> Pid       [CapSpawn]
   ArgsPtr = ArgCount NUL-terminated strings back to back, ArgsLen <= 1024
3  WaitProcess(Pid, StatusPtr, TimeoutMs) -> Pid                         Pid -1 = any child
4  KillProcess(Pid, Code)                                                [CapKill, unless it is own child]
5  GetProcessId()
6  GetParentId()
7  YieldProcess()
8  SleepMs(Ms)
9  GetUptimeMs() -> u64

## Memory
10 NewMemory(Size, Flags) -> Addr                  Flags: bit0 Write (read always), bit1 Execute; Write+Execute is rejected
11 DeleteMemory(Addr, Size)
12 ProtectMemory(Addr, Size, Flags)

## Console
13 WriteConsole(BufPtr, Len) -> Len                [CapConsole]
14 ReadConsoleKey(TimeoutMs) -> KeyCode            [CapConsole] ASCII, or >= 0x100 for special keys; ErrAgain if poll finds nothing

## Documents (files)
20 NewDocument(PathPtr, Mode)
21 OpenDocument(PathPtr, Flags) -> Handle          Flags: Read 1, Write 2, Append 4, Truncate 8
22 CloseDocument(Handle)
23 ReadDocument(Handle, BufPtr, Len) -> bytes      0 = end of document
24 WriteDocument(Handle, BufPtr, Len) -> bytes
25 SeekDocument(Handle, Offset, Whence) -> offset  Whence: 0 start, 1 current, 2 end
26 ResizeDocument(Handle, Size)
27 DeleteDocument(PathPtr)
28 RenameDocument(FromPtr, ToPtr)
29 StatDocument(PathPtr, InfoPtr)
30 SyncDocument(Handle)

## Archives (folders)
40 NewArchive(PathPtr, Mode)
41 DeleteArchive(PathPtr)                          ErrNotEmpty if it has entries
42 ListArchive(PathPtr, Index, EntryPtr)           ErrNoEnt after the last entry
43 StatArchive(PathPtr, InfoPtr)
44 RenameArchive(FromPtr, ToPtr)

## IPC
50 NewChannel(HandlesOutPtr)                       writes two i32 handles
51 SendMessage(Handle, MsgPtr, Len, AttachHandle, TimeoutMs)    AttachHandle -1 = none; it is moved to the receiver
52 ReceiveMessage(Handle, BufPtr, Cap, TimeoutMs, AttachedOutPtr) -> Len    ErrDead if the peer closed and the queue is empty
53 CloseHandle(Handle)
54 NewSharedMemory(Size) -> Handle
55 MapSharedMemory(Handle, Flags) -> Addr
56 UnmapSharedMemory(Addr)
57 NewEvent() -> Handle
58 SignalEvent(Handle)
59 WaitHandles(HandlesPtr, Count, TimeoutMs) -> Index of a ready handle (Count <= 16)
60 RegisterService(NamePtr) -> ListenerHandle      [CapService] name max 31 bytes
61 ConnectService(NamePtr) -> ChannelHandle
62 AcceptConnection(ListenerHandle, TimeoutMs) -> ChannelHandle
63 UnregisterService(NamePtr)

## Drivers and system
70 WaitIrq(Irq, TimeoutMs)                          [CapIrq]
71 MapPhysical(PhysAddr, Size, Flags) -> Addr       [CapMapPhys]
72 ReadPort(Port, Size) -> Value                    [CapIoPorts] Size 1, 2 or 4
73 WritePort(Port, Size, Value)                     [CapIoPorts]
80 GetSystemInfo(InfoPtr)
81 ShutdownSystem(Mode)                             [CapSystem] 0 power off, 1 reboot

## Structs (packed, little endian)
DocumentInfo { u64 Inode; u16 Mode; u16 Flags; u32 Uid; u32 Gid; u32 Links; u64 Size; u64 Blocks;
               u64 Atime, Mtime, Ctime, Crtime; char ExtTag[8]; }
ArchiveEntry { u64 Inode; u8 FileType; char Name[256]; }
SystemInfo   { u32 VersionMajor, VersionMinor; u64 TotalRam, FreeRam; u32 ProcessCount; u32 CpuCount; }