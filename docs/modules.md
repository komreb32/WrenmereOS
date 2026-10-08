# Kernel modules (.exf Kind=3, run in ring 0)

Region: 0xFFFFFFFFC0000000 and up, 16 slots of 4 MiB. Slot N starts at 0xFFFFFFFFC0000000 + N*0x400000.
Each module is linked at its slot base and sees the kernel through ld --just-symbols=build/kernel/kernel.elf.
Kernel flags apply (-mcmodel=kernel). Sections are 4 KiB aligned; segments are never W+X. No relocations in v1.
Slots: 0 ps2kbd, 1 serial.

Exported symbol (object, size 184): ModuleInfo
 char Name[32]; u32 Version; u32 DepCount; char Deps[4][32]; int (*Init)(void); void (*Exit)(void);

Loader: reads /System/Config/modules.list (one path per line, '#' comments) in order. Modules live in /System/Modules/, max 256 KiB.
Dependencies are checked by Name. Init returning a negative value unloads the module.