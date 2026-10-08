/* SPDX-License-Identifier: BSD-3-Clause */
// ATA bus-master DMA: the physical-region descriptor table, a 128 KiB
// staging buffer aligned to 64 KiB, and the READ/WRITE DMA (EXT) transfers
// driven through the PCI IDE bus-master registers of pciIde.c. On any
// failure the error is returned unchanged so the caller can fall back to the
// PIO layer in ataPio.c.
#include <errors.h>
#include <kernel/core/timer.h>
#include <kernel/drivers/hdd/ata.h>
#include <kernel/drivers/hdd/pciIde.h>
#include <kernel/io/io.h>
#include <kernel/mm/page_alloc.h>
#include <memlayout.h>

// Completion wait of ataIrq.c: interrupt mode with a polling fallback.
// ata.h does not declare it yet, so its intended consumer restates it here.
int AtaWaitIrq(const AtaChannel *Channel, uint32_t TimeoutMs);

// Generous deadline for a whole DMA command, including spin-up.
#define AtaDmaTimeoutMs 5000u

// DMA command codes: READ/WRITE DMA for the 28-bit command set and their EXT
// (48-bit) forms.
#define AtaCmdReadDma      0xC8u
#define AtaCmdReadDmaExt   0x25u
#define AtaCmdWriteDma     0xCAu
#define AtaCmdWriteDmaExt  0x35u

// Device/head register base: bits 7 and 5 fixed to one and bit 6 set for LBA
// mode; the slave bit is OR-ed in per drive.
#define AtaDmaDevHeadBase 0xE0u

// Exclusive upper bound of the 28-bit command set.
#define AtaDmaLba28Limit (1ull << 28)

// Staging buffer: 128 KiB of data (256 sectors of 512 bytes) in contiguous
// frames, aligned to 64 KiB so no descriptor entry crosses a 64 KiB span.
#define AtaDmaPageSize   4096u
#define AtaDmaDataPages  32u
#define AtaDmaDataBytes  (AtaDmaDataPages * AtaDmaPageSize)
#define AtaDmaPrdPages   1u // the 256-byte table fits one frame
#define AtaDmaAlign      0x10000u
#define AtaDmaAlignPages (AtaDmaAlign / AtaDmaPageSize)
#define AtaDmaLimit      0x100000000ull // descriptor addresses are 32-bit

// Sector size assumed before the drive is identified.
#define AtaDmaSectorSize 512u

// One physical-region descriptor: a 32-bit physical address, a 16-bit byte
// count and a 16-bit flag word whose top bit marks the end of the table. All
// three fields are naturally aligned, so the entry is exactly 8 bytes.
typedef struct
{
    uint32_t PhysAddr;
    uint16_t ByteCount;
    uint16_t Flags;
} AtaPrdEntry;

#define AtaPrdEnd 0x8000u // end-of-table flag (bit 15 of the flag word)

// Located controller and its cache state: 0 unknown, 1 ready, -1 failed.
static PciIdeController IdeController;
static int IdeState;

// Physical addresses of the staging buffer and of the descriptor table.
static uint64_t DmaDataPhys;
static uint64_t DmaPrdPhys;

// Reserve the 64 KiB-aligned staging buffer from the contiguous frame
// allocator. The head and tail frames the alignment leaves over are returned
// to the pool, so only the 128 KiB of data plus the table stay allocated.
static int AtaDmaEnsureBuffer(void)
{
    uint32_t TotalPages = AtaDmaDataPages + AtaDmaPrdPages + AtaDmaAlignPages;
    uint64_t Raw;
    uint64_t Aligned;
    uint32_t Offset;
    uint32_t Used;

    if (DmaDataPhys != 0)
        return ErrOk;
    Raw = PageAllocPages(TotalPages);
    if (Raw == 0)
        return ErrIo;
    Aligned = (Raw + AtaDmaAlign - 1u) & ~(uint64_t)(AtaDmaAlign - 1u);
    if (Aligned + AtaDmaDataBytes + AtaDmaPrdPages * AtaDmaPageSize >
        AtaDmaLimit) {
        PageFreePages(Raw, TotalPages); // unreachable with 32-bit descriptors
        return ErrIo;
    }
    Offset = (uint32_t)((Aligned - Raw) / AtaDmaPageSize);
    if (Offset != 0)
        PageFreePages(Raw, Offset); // frames before the aligned base
    Used = Offset + AtaDmaDataPages + AtaDmaPrdPages;
    if (TotalPages > Used)
        PageFreePages(Raw + (uint64_t)Used * AtaDmaPageSize, TotalPages - Used);

    DmaDataPhys = Aligned;
    DmaPrdPhys = Aligned + AtaDmaDataBytes;
    return ErrOk;
}

// Locate the IDE controller once and open its bus-master interface. The
// result is cached so a machine without one is only probed a single time.
static int AtaDmaEnsureController(void)
{
    if (IdeState > 0)
        return ErrOk;
    if (IdeState < 0)
        return ErrIo;
    if (PciIdeFind(&IdeController) != ErrOk) {
        IdeState = -1;
        return ErrIo;
    }
    if (PciIdeEnableBusMaster(&IdeController) != ErrOk) {
        IdeState = -1;
        return ErrIo;
    }
    IdeState = 1;
    return ErrOk;
}

// Turn a failed wait into an error code, exactly as the PIO layer does:
// floating bus or ERR/DF is ErrIo and consumes the error register, a wait
// that ran out keeps FailCode.
static int AtaDmaStatus(const AtaChannel *Channel, int FailCode)
{
    uint8_t Status = AtaReadStatus(Channel);

    if (Status == 0xFF)
        return ErrIo;
    if (Status & (AtaStatusErr | AtaStatusDfy)) {
        (void)InByte((uint16_t)(Channel->CmdBase + AtaRegError));
        return ErrIo;
    }
    return FailCode;
}

// Select the drive with a complete device/head value; AtaSelectDrive only
// programs 0x40|slave, so the high LBA bits of a 28-bit command need this
// second write.
static void AtaDmaSelect(const AtaDrive *Drive, uint8_t DevHead)
{
    const AtaChannel *Channel = Drive->Channel;

    AtaSelectDrive(Channel, Drive->Slave);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegDevice), DevHead);
    AtaDelay400ns(Channel);
}

// Fill the descriptor table for one region: one 8-byte entry per 4 KiB page,
// so no entry ever crosses a 64 KiB boundary. Bytes is a positive multiple
// of the sector size and at most the staging buffer length.
static void AtaDmaBuildPrd(uint64_t PhysBase, uint32_t Bytes)
{
    AtaPrdEntry *Prd = (AtaPrdEntry *)PhysToVirt(DmaPrdPhys);
    uint32_t Offset = 0;
    uint32_t Index = 0;

    while (Offset < Bytes) {
        uint32_t Left = Bytes - Offset;
        uint32_t Length = (Left < AtaDmaPageSize) ? Left : AtaDmaPageSize;

        Prd[Index].PhysAddr = (uint32_t)(PhysBase + Offset);
        Prd[Index].ByteCount = (uint16_t)Length;
        Prd[Index].Flags = 0;
        Offset += Length;
        Index++;
    }
    Prd[Index - 1].Flags |= AtaPrdEnd; // mark the last entry as the end
}

// Move Count bytes between the caller and the staging buffer.
static void AtaDmaCopy(void *Dst, const void *Src, uint64_t Count)
{
    __asm__ volatile ("rep movsb"
                      : "+D"(Dst), "+S"(Src), "+c"(Count)
                      :
                      : "memory");
}

// Wait until the bus master drops its Active bit. The error bit is checked
// last so a failed transfer is reported even after the engine stops.
static int AtaDmaWaitActive(const PciIdeController *Controller, uint8_t Channel,
                            uint32_t TimeoutMs)
{
    uint64_t Deadline = TimerUptimeMs() + TimeoutMs;

    for (;;) {
        uint8_t Status = PciIdeStatusRead(Controller, Channel);

        if ((Status & PciIdeStatusActive) == 0)
            return (Status & PciIdeStatusError) ? ErrIo : ErrOk;
        if (TimerUptimeMs() >= Deadline)
            return ErrTimeout;
    }
}

// Program the task file for one DMA command and issue it. LBA48 writes the
// high bytes of the count and LBA registers first, as the spec requires.
static void AtaDmaIssue(const AtaDrive *Drive, uint64_t Lba, uint32_t Count,
                        int Use48, uint8_t Cmd)
{
    const AtaChannel *Channel = Drive->Channel;
    uint8_t DevHead = (uint8_t)(AtaDmaDevHeadBase |
                               (Drive->Slave ? AtaDeviceSlave : 0u));

    if (!Use48)
        DevHead |= (uint8_t)((Lba >> 24) & 0x0Fu);
    AtaDmaSelect(Drive, DevHead);
    if (Use48) {
        OutByte((uint16_t)(Channel->CmdBase + AtaRegSectorCount),
                (uint8_t)(Count >> 8));
        OutByte((uint16_t)(Channel->CmdBase + AtaRegLba0), (uint8_t)(Lba >> 24));
        OutByte((uint16_t)(Channel->CmdBase + AtaRegLba1), (uint8_t)(Lba >> 32));
        OutByte((uint16_t)(Channel->CmdBase + AtaRegLba2), (uint8_t)(Lba >> 40));
    }
    OutByte((uint16_t)(Channel->CmdBase + AtaRegSectorCount), (uint8_t)Count);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegLba0), (uint8_t)Lba);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegLba1), (uint8_t)(Lba >> 8));
    OutByte((uint16_t)(Channel->CmdBase + AtaRegLba2), (uint8_t)(Lba >> 16));
    OutByte((uint16_t)(Channel->CmdBase + AtaRegCommand), Cmd);
    AtaDelay400ns(Channel);
}

// Run one chunk (already staged in the DMA buffer) end to end: clear the
// latched bus-master bits, load the descriptor table, program the direction,
// issue the command, start the engine, wait and stop it, then read the final
// status. Returns ErrOk or the first error seen.
static int AtaDmaTransferChunk(const AtaDrive *Drive, uint64_t Lba,
                               uint32_t Count, int IsWrite, int Use48)
{
    const AtaChannel *Channel = Drive->Channel;
    const PciIdeController *Controller = &IdeController;
    uint8_t Ch = (uint8_t)Channel->Number;
    uint8_t Command = IsWrite ? 0u : PciIdeCmdRead;
    uint32_t Bytes = Count * Drive->SectorSize;
    uint8_t Cmd = IsWrite
        ? (Use48 ? AtaCmdWriteDmaExt : AtaCmdWriteDma)
        : (Use48 ? AtaCmdReadDmaExt : AtaCmdReadDma);
    int Rc;

    // Clear the interrupt and error latches, then point the engine at a fresh
    // descriptor table with the direction set and the start bit still clear.
    PciIdeStatusWrite(Controller, Ch,
                      (uint8_t)(PciIdeStatusIrq | PciIdeStatusError));
    AtaDmaBuildPrd(DmaDataPhys, Bytes);
    PciIdePrdWrite(Controller, Ch, (uint32_t)DmaPrdPhys);
    PciIdeCommandWrite(Controller, Ch, Command);

    if (AtaWaitNotBusy(Channel, AtaDmaTimeoutMs) != 0)
        return AtaDmaStatus(Channel, ErrTimeout);

    AtaDmaIssue(Drive, Lba, Count, Use48, Cmd);

    // Start the transfer; the direction bit was set with the engine stopped.
    PciIdeCommandWrite(Controller, Ch, (uint8_t)(Command | PciIdeCmdStart));

    Rc = AtaWaitIrq(Channel, AtaDmaTimeoutMs);
    if (Rc != ErrOk) {
        PciIdeCommandWrite(Controller, Ch, 0); // stop the engine
        return (Rc == ErrTimeout) ? AtaDmaStatus(Channel, ErrTimeout) : Rc;
    }
    Rc = AtaDmaWaitActive(Controller, Ch, AtaDmaTimeoutMs);
    PciIdeCommandWrite(Controller, Ch, 0); // stop the engine
    if (Rc != ErrOk)
        return Rc;
    if (AtaWaitNotBusy(Channel, AtaDmaTimeoutMs) != 0)
        return AtaDmaStatus(Channel, ErrTimeout);
    return AtaDmaStatus(Channel, ErrOk);
}

// Shared body of AtaDmaRead/AtaDmaWrite: validate the range against the
// drive's sector count, pick the command set, stage each 128 KiB chunk in the
// DMA buffer, run it and copy the data across. A failure is returned as-is so
// the caller can retry the same request through the PIO layer.
static int AtaDmaRequest(const AtaDrive *Drive, uint64_t Lba, uint64_t Count,
                         void *Buffer, int IsWrite)
{
    const AtaChannel *Channel;
    uint32_t SectorBytes;
    uint32_t SectorsPerChunk;
    uint64_t Cur = Lba;
    uint64_t Left = Count;
    uint8_t *Ptr = (uint8_t *)Buffer;
    int Use48;
    int Rc;

    if (Drive == 0)
        return ErrIo;
    Channel = Drive->Channel;
    if (Channel == 0)
        return ErrIo;
    if ((Drive->Flags & (AtaDrivePresent | AtaDriveDma)) !=
        (AtaDrivePresent | AtaDriveDma))
        return ErrUnsupported; // no DMA: the caller uses PIO
    if (Count == 0)
        return ErrOk; // nothing to transfer
    if (Buffer == 0)
        return ErrIo;
    if (Drive->SectorCount == 0 || Lba >= Drive->SectorCount ||
        Count > Drive->SectorCount - Lba)
        return ErrIo; // outside the advertised capacity

    // DMA uses the 48-bit command set when the drive offers it; otherwise an
    // LBA28 command, which cannot reach a request beyond 2^28.
    Use48 = (Drive->Flags & AtaDriveLba48) != 0;
    if (!Use48 && (Lba >= AtaDmaLba28Limit || Count > AtaDmaLba28Limit - Lba))
        return ErrUnsupported; // the caller uses PIO for this one

    Rc = AtaDmaEnsureController();
    if (Rc != ErrOk)
        return Rc;
    Rc = AtaDmaEnsureBuffer();
    if (Rc != ErrOk)
        return Rc;

    SectorBytes = Drive->SectorSize ? Drive->SectorSize : AtaDmaSectorSize;
    SectorsPerChunk = AtaDmaDataBytes / SectorBytes;

    while (Left != 0) {
        uint32_t Chunk = (Left < SectorsPerChunk) ? (uint32_t)Left
                                                  : SectorsPerChunk;
        uint32_t Bytes = Chunk * SectorBytes;
        uint8_t *Dma = (uint8_t *)PhysToVirt(DmaDataPhys);

        if (IsWrite)
            AtaDmaCopy(Dma, Ptr, Bytes); // caller to the staging buffer
        Rc = AtaDmaTransferChunk(Drive, Cur, Chunk, IsWrite, Use48);
        if (Rc != ErrOk)
            return Rc; // the caller falls back to PIO
        if (!IsWrite)
            AtaDmaCopy(Ptr, Dma, Bytes); // staging buffer to the caller
        Cur += Chunk;
        Left -= Chunk;
        Ptr += Bytes;
    }
    return ErrOk;
}

int AtaDmaRead(const AtaDrive *Drive, uint64_t Lba, uint64_t Count,
               void *Buffer)
{
    return AtaDmaRequest(Drive, Lba, Count, Buffer, 0);
}

int AtaDmaWrite(const AtaDrive *Drive, uint64_t Lba, uint64_t Count,
                const void *Buffer)
{
    // The transfer only reads the buffer; AtaDmaRequest hands it to the
    // staging copy as const data.
    return AtaDmaRequest(Drive, Lba, Count, (void *)(uintptr_t)Buffer, 1);
}
