/* SPDX-License-Identifier: BSD-3-Clause */
#include <bootinfo.h>
#include <lib/stdio.h>
#include <fs2.h>
#include <kernel/core/panic.h>
#include <kernel/core/timer.h>
#include <kernel/drivers/hdd/hdd.h>
#include <kernel/drivers/keyboard.h>
#include <kernel/gdt/gdt.h>
#include <kernel/idt/idt.h>
#include <kernel/mm/mm.h>
#include <memlayout.h>
#include <percpu.h>
#include <process.h>
#include <proc/sched.h>
#include <syscall.h>
#include "../archive/exf/exf.h"

#define VgaRow24 ((volatile uint16_t *)(uintptr_t)(0xB8000 + 24 * 80 * 2))

static void WriterA(uint64_t Arg)
{
    (void)Arg;
    for (;;) {
        VgaRow24[0] = (uint16_t)((0x0A << 8) | 'A');
        SleepCurrentMs(1000);
    }
}

static void WriterB(uint64_t Arg)
{
    (void)Arg;
    for (;;) {
        VgaRow24[1] = (uint16_t)((0x0C << 8) | 'B');
        SleepCurrentMs(1000);
    }
}

static uint8_t ChunkBuffer[4096];

static const char *DetectFilesystem(BlockDevice *dev, uint64_t *StartLbaOut)
{
    static uint8_t sector[512];

    if (dev == 0 || dev->Read == 0)
        return "Unknown";

    // Check 2fs at LBA 2048 (unpartitioned raw disk offset)
    if (dev->Read(dev, 2048, 1, sector) == ErrOk) {
        uint64_t magic = *(uint64_t *)sector;
        if (magic == 0x4E4552572D534632ULL) { // "2FS-WREN"
            if (StartLbaOut != 0)
                *StartLbaOut = 2048;
            return "2fs";
        }
    }

    // Check 2fs at LBA 0 (partition offset)
    if (dev->Read(dev, 0, 1, sector) == ErrOk) {
        uint64_t magic = *(uint64_t *)sector;
        if (magic == 0x4E4552572D534632ULL) {
            if (StartLbaOut != 0)
                *StartLbaOut = 0;
            return "2fs";
        }
    }

    return "Unknown";
}

void CpuFeaturesEnable(void);

// No C prologue may touch the BIOS stack before we switch RSP. A normal
// call below establishes SysV's 16-byte stack alignment for KernelMain.
__attribute__((naked, noreturn)) void KernelEntry(const BootInfo *Info)
{
    CpuFeaturesEnable();
    SyscallInit();
    __asm__ volatile (
        "cli\n\t"
        "leaq KernelBootStackTop(%rip), %rsp\n\t"
        "xorq %rbp, %rbp\n\t"
        "call KernelMain\n\t"
        "1: hlt\n\t"
        "jmp 1b\n\t"
    );
}

__attribute__((used, noinline, noreturn)) static void KernelMain(const BootInfo *Info)
{
    int MmUp;

    // Stage2 passes a physical BootInfo pointer. Never dereference it until
    // it has been converted to the direct-map alias.
    if (Info == 0)
        Panic("invalid boot info pointer", 0);
    Info = (const BootInfo *)PhysToVirt((uint64_t)(uintptr_t)Info);
    if (Info->Magic != BootInfoMagic)
        Panic("invalid boot info magic", 0);

    IrqDisable();
    KClear();
    KPrintln("Wrenmere OS");
    KPrintln("");

    KStatus("GDT loaded: kernel code 0x08, data 0x10", GdtInit() == 0);
    PerCpuInit();
    KStatus("IDT loaded: 256 gates, 48 stubs", IdtStart() == 0);
    KStatus("PIC remapped: IRQ 0-15 -> vectors 0x20-0x2F", PicInit() == 0);
    KStatus("PIT timer running at 100 Hz on IRQ0", TimerStart(100) == 0);
    KStatus("PS/2 keyboard ready on IRQ1", KeyboardStart() == 0);
    MmUp = MmInit() == 0;
    KStatus("MM paging with 4KB pages", MmUp);
    if (MmUp)
    {
        KInfoDec("total pages",          MmTotalPages());
        KInfoDec("total RAM (MiB)",      MmTotalRamBytes()  / 1048576ULL);
        KInfoDec("free  RAM (MiB)",      MmUsableRam()      / 1048576ULL);
        KInfoDec("used  RAM (KiB)",     (MmUsedRam()       + 1023ULL) / 1024ULL);
    }

    // Enumerate the ATA disks, bring up DMA where possible and register the
    // block devices ("0:/", "1:/") before the keyboard loop enables interrupts.
    HddInit();

    BlockDevice *bootDev = 0;
    uint64_t bootLba = 2048;
    char bootDevName[8] = "";

    // Show the filesystem of all detected disks and partitions
    for (int i = 0; i < HddCount(); i++) {
        int hasParts = 0;
        for (int p = 0; p < HddPartitionCount(); p++) {
            const char *pname = HddPartitionName(p);
            if (pname != 0 && pname[0] == (char)('0' + i)) {
                hasParts = 1;
                uint64_t startLba = 0;
                BlockDevice *pdev = (BlockDevice *)HddPartitionGet(p);
                const char *fs = DetectFilesystem(pdev, &startLba);
                KPrint(pname);
                KPrint(": ");
                KPrintln(fs);
                if (bootDev == 0 && fs[0] == '2' && fs[1] == 'f' && fs[2] == 's') {
                    bootDev = pdev;
                    bootLba = startLba;
                    int k = 0;
                    while (pname[k] != '\0' && k < 7) {
                        bootDevName[k] = pname[k];
                        k++;
                    }
                    bootDevName[k] = '\0';
                }
            }
        }

        if (!hasParts) {
            char dname[5];
            dname[0] = (char)('0' + i);
            dname[1] = ':';
            dname[2] = '/';
            dname[3] = '\0';
            uint64_t startLba = 0;
            BlockDevice *ddev = (BlockDevice *)HddGet(i);
            const char *fs = DetectFilesystem(ddev, &startLba);
            KPrint(dname);
            KPrint(": ");
            KPrintln(fs);
            if (bootDev == 0 && fs[0] == '2' && fs[1] == 'f' && fs[2] == 's') {
                bootDev = ddev;
                bootLba = startLba;
                int k = 0;
                while (dname[k] != '\0' && k < 7) {
                    bootDevName[k] = dname[k];
                    k++;
                }
                bootDevName[k] = '\0';
            }
        }
    }

    if (bootDev != 0) {
        int rc = Fs2Mount(bootDev, bootLba);
        char mountMsg[32] = "mount 2fs on ";
        int mIdx = 13;
        int bIdx = 0;
        while (bootDevName[bIdx] != '\0' && mIdx < 30)
            mountMsg[mIdx++] = bootDevName[bIdx++];
        mountMsg[mIdx] = '\0';

        KStatus(mountMsg, rc == 0);
        if (rc != 0) {
            KPrint("Fs2Mount error: ");
            KPrintDec((uint64_t)(-rc));
            KPrintln("");
        } else {
            char label[64];
            uint64_t totalBlocks = 0;
            uint64_t freeBlocks = 0;
            int irc = Fs2Info(label, &totalBlocks, &freeBlocks);
            if (irc != 0) {
                KPrint("Fs2Info error: ");
                KPrintDec((uint64_t)(-irc));
                KPrintln("");
            } else {
                KPrint("  Volume label: ");
                KPrintln(label);
                KInfoDec("free blocks", freeBlocks);
                KInfoDec("total blocks", totalBlocks);
            }

            struct Fs2Stat st;
            int src = Fs2Stat("/System/kernel.exf", &st);
            if (src != 0) {
                KPrint("Fs2Stat(\"/System/kernel.exf\") error: ");
                KPrintDec((uint64_t)(-src));
                KPrintln("");
            } else {
                uint64_t offset = 0;
                uint32_t crc = 0xFFFFFFFFu;
                int readErr = 0;
                while (offset < st.Size) {
                    uint64_t toRead = sizeof(ChunkBuffer);
                    if (offset + toRead > st.Size)
                        toRead = st.Size - offset;

                    uint64_t bytesRead = 0;
                    int rrc = Fs2Read("/System/kernel.exf", offset, ChunkBuffer, toRead, &bytesRead);
                    if (rrc != 0) {
                        KPrint("Fs2Read error: ");
                        KPrintDec((uint64_t)(-rrc));
                        KPrintln("");
                        readErr = 1;
                        break;
                    }
                    if (bytesRead == 0)
                        break;

                    crc = Crc32Update(crc, ChunkBuffer, bytesRead);
                    offset += bytesRead;
                }
                if (!readErr) {
                    crc ^= 0xFFFFFFFFu;
                    KPrint("  /System/kernel.exf bytes: ");
                    KPrintDec(st.Size);
                    KPrint(", CRC32: 0x");
                    KPrintHex(crc);
                    KPrintln("");
                }
            }

            int urc = Fs2Unmount();
            KStatus("unmount 2fs", urc == 0);
            if (urc != 0) {
                KPrint("Fs2Unmount error: ");
                KPrintDec((uint64_t)(-urc));
                KPrintln("");
            }
        }
    }

    SchedInit();
    {
        Process *Kp = CurrentProcess();
        Thread *Ta;
        Thread *Tb;

        Ta = ThreadCreateKernel(Kp, (uint64_t)(uintptr_t)WriterA, 0);
        Tb = ThreadCreateKernel(Kp, (uint64_t)(uintptr_t)WriterB, 0);
        if (Ta != 0)
            SchedAdd(Ta);
        if (Tb != 0)
            SchedAdd(Tb);
    }

    IrqEnable();

    for (;;)
        Yield();
}
