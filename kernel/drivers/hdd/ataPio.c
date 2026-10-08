/* SPDX-License-Identifier: BSD-3-Clause */
// ATA PIO sector transfers: LBA28/LBA48 task-file setup, per-sector DRQ
// polling with rep insw/rep outsw, retries with a soft reset between
// attempts, the write-cache flush and the BlockDevice adapters.
#include <errors.h>
#include <kernel/drivers/hdd/ata.h>
#include <kernel/io/io.h>

// Deadline for each BSY/DRQ poll while a command runs.
#define AtaPioTimeoutMs 1000u
// FLUSH CACHE can take seconds on a real drive with a full write cache.
#define AtaPioFlushTimeoutMs 5000u
// Retries after the first attempt; AtaSoftReset runs between attempts.
#define AtaPioRetries 3u

// Exclusive upper bound of the 28-bit command set.
#define AtaPioLba28Limit (1ull << 28)
// Sectors per command: the 8-bit count register wraps 256 to 0 and the
// 16-bit EXT count register wraps 65536 to 0.
#define AtaPioChunkLba28 256u
#define AtaPioChunkLba48 65536u

// Device/head value with the fixed bits (7 and 5) and LBA mode set.
#define AtaPioDevHeadBase 0xE0u

// 16-bit words per sector for the rep insw/rep outsw transfers.
static uint32_t PioWords(const AtaDrive *Drive)
{
    uint32_t Bytes = Drive->SectorSize;

    if (Bytes < 2)
        Bytes = 512; // not identified yet: the common sector size
    return Bytes / 2;
}

// Turn a failed wait into an error code. Floating bus or ERR/DF is
// ErrIo and consumes the error register; a wait that ran out or a
// completed command without ERR/DF keeps FailCode instead.
static int PioStatus(const AtaChannel *Channel, int FailCode)
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

// Select the drive with a complete device/head value: bits 7 and 5 are
// fixed to 1, bit 6 selects LBA mode and, for LBA28, bits 3:0 carry
// LBA 27:24. AtaSelectDrive only programs 0x40|slave, so the high LBA
// bits never reach the task file without this second write.
static void PioSelect(const AtaDrive *Drive, uint8_t DevHead)
{
    const AtaChannel *Channel = Drive->Channel;

    AtaSelectDrive(Channel, Drive->Slave);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegDevice), DevHead);
    AtaDelay400ns(Channel);
}

// Program the task file for one command and issue it. LBA48 writes the
// high bytes of the sector count and LBA registers before the low ones
// as the ATA spec requires; LBA28 writes each register once and parks
// LBA 27:24 in the device/head register.
static void PioIssue(const AtaDrive *Drive, uint64_t Lba, uint32_t Count,
                     int Use48, uint8_t Cmd)
{
    const AtaChannel *Channel = Drive->Channel;
    uint8_t DevHead = (uint8_t)(AtaPioDevHeadBase |
                               (Drive->Slave ? AtaDeviceSlave : 0u));

    if (!Use48)
        DevHead |= (uint8_t)((Lba >> 24) & 0x0Fu);
    PioSelect(Drive, DevHead);
    if (Use48) {
        OutByte((uint16_t)(Channel->CmdBase + AtaRegSectorCount),
                (uint8_t)(Count >> 8));
        OutByte((uint16_t)(Channel->CmdBase + AtaRegLba0),
                (uint8_t)(Lba >> 24));
        OutByte((uint16_t)(Channel->CmdBase + AtaRegLba1),
                (uint8_t)(Lba >> 32));
        OutByte((uint16_t)(Channel->CmdBase + AtaRegLba2),
                (uint8_t)(Lba >> 40));
    }
    OutByte((uint16_t)(Channel->CmdBase + AtaRegSectorCount),
            (uint8_t)Count);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegLba0), (uint8_t)Lba);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegLba1), (uint8_t)(Lba >> 8));
    OutByte((uint16_t)(Channel->CmdBase + AtaRegLba2), (uint8_t)(Lba >> 16));
    OutByte((uint16_t)(Channel->CmdBase + AtaRegCommand), Cmd);
    // Status must not be read for 400 ns after the command write; the
    // delay also keeps PioCommand from sampling a stale pre-command
    // status before the device raises BSY.
    AtaDelay400ns(Channel);
}

// Issue one command and move Count sectors across the data port, one
// DRQ handshake per sector, then check the completion status.
static int PioCommand(const AtaDrive *Drive, uint64_t Lba, uint32_t Count,
                      int Use48, int IsWrite, uint8_t *Buffer)
{
    const AtaChannel *Channel = Drive->Channel;
    uint16_t Data = (uint16_t)(Channel->CmdBase + AtaRegData);
    uint32_t Words = PioWords(Drive);
    uint32_t Sector;
    uint8_t Cmd = IsWrite
        ? (Use48 ? AtaCmdWriteSectorsExt : AtaCmdWriteSectors)
        : (Use48 ? AtaCmdReadSectorsExt : AtaCmdReadSectors);

    PioIssue(Drive, Lba, Count, Use48, Cmd);
    for (Sector = 0; Sector < Count; Sector++) {
        if (AtaWaitDrq(Channel, AtaPioTimeoutMs) != 0)
            return PioStatus(Channel, ErrTimeout);
        if (IsWrite)
            OutWordBuffer(Data, Buffer, Words);
        else
            InWordBuffer(Data, Buffer, Words);
        Buffer += Words * 2;
    }
    if (AtaWaitNotBusy(Channel, AtaPioTimeoutMs) != 0)
        return PioStatus(Channel, ErrTimeout);
    return PioStatus(Channel, ErrOk);
}

// Run the whole request as a sequence of commands: 256 sectors per
// LBA28 command, 65536 per LBA48 command. The range check guarantees
// that no LBA28 chunk crosses the 28-bit boundary.
static int PioRun(const AtaDrive *Drive, uint64_t Lba, uint64_t Count,
                  uint8_t *Buffer, int IsWrite, int Use48)
{
    uint64_t Cur = Lba;
    uint64_t Left = Count;
    uint32_t SectorBytes = PioWords(Drive) * 2;

    while (Left != 0) {
        uint64_t Chunk = Use48 ? AtaPioChunkLba48 : AtaPioChunkLba28;
        int Rc;

        if (Chunk > Left)
            Chunk = Left;
        Rc = PioCommand(Drive, Cur, (uint32_t)Chunk, Use48, IsWrite, Buffer);
        if (Rc != ErrOk)
            return Rc;
        Cur += Chunk;
        Left -= Chunk;
        Buffer += Chunk * SectorBytes;
    }
    return ErrOk;
}

// Shared body of AtaPioRead/AtaPioWrite: validate the range against the
// drive's sector count, pick the command set and retry the full request
// up to AtaPioRetries times with AtaSoftReset between attempts.
static int PioRequest(const AtaDrive *Drive, uint64_t Lba, uint64_t Count,
                      void *Buffer, int IsWrite)
{
    const AtaChannel *Channel;
    int Use48;
    int Rc = ErrIo;
    uint32_t Attempt;

    if (Drive == 0)
        return ErrIo;
    Channel = Drive->Channel;
    if (Channel == 0)
        return ErrIo;
    if (Count == 0)
        return ErrOk; // nothing to transfer
    if (Buffer == 0)
        return ErrIo;
    if (Drive->SectorCount == 0 ||
        Lba >= Drive->SectorCount ||
        Count > Drive->SectorCount - Lba)
        return ErrIo; // outside the advertised capacity

    // LBA28 only when the drive lacks the 48-bit command set and the
    // whole request fits below 2^28; anything else needs LBA48, which
    // such a drive does not have.
    Use48 = (Drive->Flags & AtaDriveLba48) != 0;
    if (!Use48 && (Lba >= AtaPioLba28Limit ||
                   Count > AtaPioLba28Limit - Lba))
        return ErrIo;

    for (Attempt = 0; Attempt <= AtaPioRetries; Attempt++) {
        if (Attempt != 0)
            (void)AtaSoftReset(Channel); // between attempts
        Rc = PioRun(Drive, Lba, Count, (uint8_t *)Buffer, IsWrite, Use48);
        if (Rc == ErrOk)
            return Rc;
    }
    return Rc;
}

int AtaPioRead(const AtaDrive *Drive, uint64_t Lba, uint64_t Count,
               void *Buffer)
{
    return PioRequest(Drive, Lba, Count, Buffer, 0);
}

int AtaPioWrite(const AtaDrive *Drive, uint64_t Lba, uint64_t Count,
                const void *Buffer)
{
    // The transfer only reads the buffer; PioRequest hands it to
    // OutWordBuffer as const data.
    int Rc = PioRequest(Drive, Lba, Count, (void *)(uintptr_t)Buffer, 1);

    if (Rc != ErrOk)
        return Rc;
    return AtaFlushCache(Drive); // the sectors must leave the cache too
}

// Flush the drive's write cache: FLUSH CACHE EXT for drives with the
// 48-bit command set (the ones this driver writes with EXT commands),
// plain FLUSH CACHE for LBA28-only drives.
int AtaFlushCache(const AtaDrive *Drive)
{
    const AtaChannel *Channel;

    if (Drive == 0)
        return ErrIo;
    Channel = Drive->Channel;
    if (Channel == 0)
        return ErrIo;
    if (AtaWaitNotBusy(Channel, AtaPioTimeoutMs) != 0)
        return PioStatus(Channel, ErrTimeout);

    PioSelect(Drive, (uint8_t)(AtaPioDevHeadBase |
                               (Drive->Slave ? AtaDeviceSlave : 0u)));
    OutByte((uint16_t)(Channel->CmdBase + AtaRegCommand),
            (uint8_t)((Drive->Flags & AtaDriveLba48) ? AtaCmdCacheFlushExt
                                                     : AtaCmdCacheFlush));
    AtaDelay400ns(Channel);
    if (AtaWaitNotBusy(Channel, AtaPioFlushTimeoutMs) != 0)
        return PioStatus(Channel, ErrTimeout);
    return PioStatus(Channel, ErrOk);
}

// BlockDevice adapters: Device->Private is the AtaDrive; they forward
// straight to the PIO layer, range checks included.
int AtaBlockRead(BlockDevice *Device, uint64_t Lba, uint64_t Count,
                 void *Buffer)
{
    if (Device == 0)
        return ErrIo;
    return AtaPioRead((const AtaDrive *)Device->Private, Lba, Count, Buffer);
}

int AtaBlockWrite(BlockDevice *Device, uint64_t Lba, uint64_t Count,
                  const void *Buffer)
{
    if (Device == 0)
        return ErrIo;
    return AtaPioWrite((const AtaDrive *)Device->Private, Lba, Count, Buffer);
}

int AtaBlockFlush(BlockDevice *Device)
{
    if (Device == 0)
        return ErrIo;
    return AtaFlushCache((const AtaDrive *)Device->Private);
}


