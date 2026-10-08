/* SPDX-License-Identifier: BSD-3-Clause */
// Hard-disk layer: IDENTIFY the ATA drives on both channels, register their
// interrupt lines, test the PCI IDE bus-master for DMA and publish every drive
// as a BlockDevice named hd0..hd3. The block adapters prefer DMA and fall back
// to the PIO layer in ataPio.c whenever DMA is unavailable or fails.
#include <errors.h>
#include <lib/stdio.h>
#include <kernel/drivers/hdd/ata.h>
#include <kernel/drivers/hdd/hdd.h>
#include <kernel/drivers/hdd/pciIde.h>
#include <kernel/io/io.h>

// ataIrq.c owns the channel interrupt setup and the nIEN arming; ata.h does
// not declare them yet, so this consumer restates the prototypes.
int AtaIrqInit(const AtaChannel *Channel);
int AtaSetIrqMode(const AtaChannel *Channel, int Enabled);

// Two channels, two drives each; at most four block devices.
#define HddChannelCount 2
#define HddDrivesPerChannel 2

// IDENTIFY DEVICE hands back 256 words; a stalled command is given a second.
#define HddIdentifyWords 256u
#define HddIdentifyTimeoutMs 1000u

// Device/head value for IDENTIFY: bits 7 and 5 fixed, no LBA bit. The slave
// bit is OR-ed in per drive.
#define HddDevHeadBase 0xA0u

// IDENTIFY string fields, in 16-bit words.
#define HddModelWord    27
#define HddModelWords   20
#define HddSerialWord   10
#define HddSerialWords  10
#define HddFirmwareWord 23
#define HddFirmwareWords 4

// Identify words of interest.
#define HddWordMultiDma 63u // low byte: multiword DMA modes
#define HddWordLba48    83u // bit 10: 48-bit LBA supported
#define HddWordUdma     88u // low byte: UDMA modes supported
#define HddWordLba28Lo  60u
#define HddWordLba28Hi  61u
#define HddWordLba48Lo  100u
#define HddWordSectorSize 106u
#define HddWordLogicalLo 117u
#define HddWordLogicalHi 118u

#define HddLba48Bit 0x0400u // word 83, bit 10
#define HddSectorSizeValid 0x8000u // word 106, bit 15
#define HddSectorSizeBig 0x1000u   // word 106, bit 12

// The two ATA channels, in the order HddInit walks them.
static const AtaChannel PrimaryChannel = {
    AtaPrimaryCmdBase, AtaPrimaryCtlBase, AtaPrimaryIrq, 0u
};
static const AtaChannel SecondaryChannel = {
    AtaSecondaryCmdBase, AtaSecondaryCtlBase, AtaSecondaryIrq, 1u
};
static const AtaChannel *const HddChannels[HddChannelCount] = {
    &PrimaryChannel, &SecondaryChannel
};

// Copy a space-padded IDENTIFY string, swapping the byte order of every word
// and trimming the trailing blanks.
static void HddIdentifyString(const uint16_t *Words, int Count, char *Out)
{
    int i;

    for (i = 0; i < Count; i++) {
        Out[i * 2] = (char)(Words[i] >> 8);
        Out[i * 2 + 1] = (char)(Words[i] & 0xFFu);
    }
    Out[Count * 2] = '\0';
    for (i = Count * 2 - 1; i >= 0 && Out[i] == ' '; i--)
        Out[i] = '\0';
}

// Highest UDMA mode the drive reports in the low byte of word 88.
static uint8_t HddHighestUdma(uint16_t Modes)
{
    uint8_t Top = 0;
    int i;

    for (i = 0; i < 8; i++) {
        if (Modes & (uint16_t)(1u << i))
            Top = (uint8_t)i;
    }
    return Top;
}

// Issue IDENTIFY DEVICE to one drive and fill its descriptor. Returns ErrOk
// when a drive answered, ErrIo when the position is empty, floating or the
// device did not hand back usable data.
static int HddIdentify(const AtaChannel *Channel, uint8_t Slave, AtaDrive *Drive)
{
    uint16_t Data[HddIdentifyWords];
    uint32_t LogicalWords;
    uint8_t Status;

    AtaSelectDrive(Channel, Slave);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegSectorCount), 0);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegLba0), 0);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegLba1), 0);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegLba2), 0);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegDevice),
            (uint8_t)(HddDevHeadBase | (Slave ? AtaDeviceSlave : 0u)));
    AtaDelay400ns(Channel);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegCommand),
            (uint8_t)AtaCmdIdentify);
    AtaDelay400ns(Channel);

    Status = AtaReadStatus(Channel);
    if (Status == 0x00u)
        return ErrIo; // no drive answers at this position
    if (Status == 0xFFu)
        return ErrIo; // floating bus
    if (AtaWaitDrq(Channel, HddIdentifyTimeoutMs) != 0)
        return ErrIo; // no data phase: absent or ATAPI

    InWordBuffer((uint16_t)(Channel->CmdBase + AtaRegData), Data,
                 (uint64_t)HddIdentifyWords);

    Drive->Channel = Channel;
    Drive->Slave = Slave;
    Drive->Flags = AtaDrivePresent;
    Drive->UdmaMode = 0;
    Drive->SectorSize = 512;
    Drive->SectorCount = 0;
    Drive->Name[0] = '\0';

    HddIdentifyString(&Data[HddModelWord], HddModelWords, Drive->Model);
    HddIdentifyString(&Data[HddSerialWord], HddSerialWords, Drive->Serial);
    HddIdentifyString(&Data[HddFirmwareWord], HddFirmwareWords, Drive->Firmware);

    if (Data[HddWordLba48] & HddLba48Bit)
        Drive->Flags |= AtaDriveLba48;
    if ((Data[HddWordMultiDma] & 0x00FFu) != 0 ||
        (Data[HddWordUdma] & 0x00FFu) != 0) {
        Drive->Flags |= AtaDriveDma;
        Drive->UdmaMode = HddHighestUdma(Data[HddWordUdma]);
    }

    if (Drive->Flags & AtaDriveLba48) {
        Drive->SectorCount = (uint64_t)Data[HddWordLba48Lo]
            | ((uint64_t)Data[HddWordLba48Lo + 1] << 16)
            | ((uint64_t)Data[HddWordLba48Lo + 2] << 32)
            | ((uint64_t)Data[HddWordLba48Lo + 3] << 48);
    } else {
        Drive->SectorCount = (uint64_t)Data[HddWordLba28Lo]
            | ((uint64_t)Data[HddWordLba28Hi] << 16);
    }

    // Logical sector size in words 117/118 when word 106 marks it valid and
    // flags a size other than 512 bytes.
    if ((Data[HddWordSectorSize] & HddSectorSizeValid) &&
        (Data[HddWordSectorSize] & HddSectorSizeBig)) {
        LogicalWords = (uint32_t)Data[HddWordLogicalLo]
            | ((uint32_t)Data[HddWordLogicalHi] << 16);
        if (LogicalWords != 0)
            Drive->SectorSize = LogicalWords * 2u;
    }

    if (Drive->SectorCount == 0)
        return ErrIo; // no usable capacity
    return ErrOk;
}

// Probe the master and slave of both channels and collect the drives that
// answered, up to Max.
int AtaDetectAll(AtaDrive *Out, int Max)
{
    int Count = 0;
    int Channel;
    int Slave;

    if (Out == 0 || Max <= 0)
        return 0;
    for (Channel = 0; Channel < HddChannelCount && Count < Max; Channel++) {
        for (Slave = 0; Slave < HddDrivesPerChannel && Count < Max; Slave++) {
            if (HddIdentify(HddChannels[Channel], (uint8_t)Slave,
                            &Out[Count]) == ErrOk)
                Count++;
        }
    }
    return Count;
}

// One registered disk: its ATA descriptor, its block-device view and the
// transfer mode chosen at startup.
typedef struct
{
    AtaDrive Drive;
    BlockDevice Block;
    int DmaAvailable;
} HddEntry;

static HddEntry Entries[HddMaxDrives];
static int HddEntryCount;

// DMA probe reads a whole 4 KiB with one or more drive sectors.
#define HddDmaProbeBytes 4096u
static uint8_t HddProbeBuffer[HddDmaProbeBytes];

// Read through DMA when it was chosen, otherwise through PIO. A DMA transfer
// that fails is retried through PIO, so a flaky bus cannot lose the request.
static int HddRead(BlockDevice *Device, uint64_t Lba, uint64_t Count,
                   void *Buffer)
{
    HddEntry *Entry = (HddEntry *)Device->Private;

    if (Entry->DmaAvailable) {
        if (AtaDmaRead(&Entry->Drive, Lba, Count, Buffer) == ErrOk)
            return ErrOk;
    }
    return AtaPioRead(&Entry->Drive, Lba, Count, Buffer);
}

static int HddWrite(BlockDevice *Device, uint64_t Lba, uint64_t Count,
                    const void *Buffer)
{
    HddEntry *Entry = (HddEntry *)Device->Private;

    if (Entry->DmaAvailable) {
        if (AtaDmaWrite(&Entry->Drive, Lba, Count, Buffer) == ErrOk)
            return ErrOk;
    }
    return AtaPioWrite(&Entry->Drive, Lba, Count, Buffer);
}

// Flush the drive's write cache; this also covers the writes issued through
// DMA, whose path leaves the cache alone.
static int HddFlush(BlockDevice *Device)
{
    HddEntry *Entry = (HddEntry *)Device->Private;

    return AtaFlushCache(&Entry->Drive);
}

// Locate the PCI IDE controller and open its bus-master interface. Returns 1
// when DMA is possible, 0 otherwise.
static int HddStartDmaController(void)
{
    PciIdeController Controller;

    if (PciIdeFind(&Controller) != ErrOk)
        return 0;
    if (PciIdeEnableBusMaster(&Controller) != ErrOk)
        return 0;
    return 1;
}

// Read one sector run through DMA to prove the bus master drives this drive;
// any failure leaves the disk in PIO mode.
static int HddProbeDma(const AtaDrive *Drive)
{
    uint32_t Sectors = HddDmaProbeBytes / Drive->SectorSize;

    if (Sectors == 0)
        Sectors = 1;
    return AtaDmaRead(Drive, 0, Sectors, HddProbeBuffer);
}

// Register the IRQ line of every channel that carries a detected drive, once.
static void HddIrqInit(const AtaDrive *Found, int Count)
{
    int i;
    int j;

    for (i = 0; i < Count; i++) {
        int Seen = 0;

        for (j = 0; j < i; j++) {
            if (Found[j].Channel == Found[i].Channel)
                Seen = 1;
        }
        if (!Seen)
            (void)AtaIrqInit(Found[i].Channel);
    }
}

// One console line per disk: name, model, capacity in MiB and mode.
static void HddPrintDisk(const HddEntry *Entry)
{
    uint64_t Bytes = Entry->Drive.SectorCount * Entry->Drive.SectorSize;
    uint64_t Mib = (Bytes + 1048575ULL) / 1048576ULL;

    KPrint(Entry->Drive.Name);
    KPrint(": ");
    if (Entry->Drive.Model[0] != '\0')
        KPrint(Entry->Drive.Model);
    else
        KPrint("(unknown)");
    KPrint(" ");
    KPrintDec(Mib);
    KPrint(" MiB ");
    KPrintln(Entry->DmaAvailable ? "DMA" : "PIO");
}

// Partition view: sub-region of a physical ATA drive.
typedef struct
{
    char Name[8]; // e.g. "0:1:/"
    uint8_t DriveIndex;
    uint8_t PartIndex;
    uint8_t Type;
    uint32_t StartLba;
    uint32_t SectorCount;
    BlockDevice Block;
} HddPartitionEntry;

static HddPartitionEntry Partitions[HddMaxPartitions];
static int PartitionCount = 0;

static int PartitionRead(BlockDevice *Device, uint64_t Lba, uint64_t Count,
                         void *Buffer)
{
    HddPartitionEntry *Part = (HddPartitionEntry *)Device->Private;

    if (Lba + Count > Part->SectorCount)
        return ErrIo;
    return HddRead(&Entries[Part->DriveIndex].Block, Part->StartLba + Lba,
                   Count, Buffer);
}

static int PartitionWrite(BlockDevice *Device, uint64_t Lba, uint64_t Count,
                          const void *Buffer)
{
    HddPartitionEntry *Part = (HddPartitionEntry *)Device->Private;

    if (Lba + Count > Part->SectorCount)
        return ErrIo;
    return HddWrite(&Entries[Part->DriveIndex].Block, Part->StartLba + Lba,
                    Count, Buffer);
}

static int PartitionFlush(BlockDevice *Device)
{
    HddPartitionEntry *Part = (HddPartitionEntry *)Device->Private;

    return HddFlush(&Entries[Part->DriveIndex].Block);
}

static void HddPrintPartition(const HddPartitionEntry *Part, uint32_t SectorSize)
{
    uint64_t Bytes = (uint64_t)Part->SectorCount * SectorSize;
    uint64_t Mib = (Bytes + 1048575ULL) / 1048576ULL;

    KPrint("  ");
    KPrint(Part->Name);
    KPrint(": ");
    KPrintDec(Mib);
    KPrint(" MiB (type 0x");
    KPrintHex(Part->Type);
    KPrintln(")");
}

static void HddDetectPartitions(int DriveIndex)
{
    uint8_t Mbr[512];
    HddEntry *Disk = &Entries[DriveIndex];
    int p;

    if (HddRead(&Disk->Block, 0, 1, Mbr) != ErrOk)
        return;

    // Check MBR boot signature 0x55, 0xAA
    if (Mbr[510] != 0x55 || Mbr[511] != 0xAA)
        return;

    for (p = 0; p < 4; p++) {
        const uint8_t *Entry = &Mbr[446 + p * 16];
        uint8_t Type = Entry[4];
        uint32_t StartLba = (uint32_t)Entry[8] | ((uint32_t)Entry[9] << 8) |
                            ((uint32_t)Entry[10] << 16) | ((uint32_t)Entry[11] << 24);
        uint32_t SectorCount = (uint32_t)Entry[12] | ((uint32_t)Entry[13] << 8) |
                               ((uint32_t)Entry[14] << 16) | ((uint32_t)Entry[15] << 24);

        if (Type == 0 || SectorCount == 0)
            continue;

        if (PartitionCount >= HddMaxPartitions)
            break;

        HddPartitionEntry *Part = &Partitions[PartitionCount];
        Part->DriveIndex = (uint8_t)DriveIndex;
        Part->PartIndex = (uint8_t)(p + 1);
        Part->Type = Type;
        Part->StartLba = StartLba;
        Part->SectorCount = SectorCount;

        // Name format: "0:1:/", "0:2:/", "1:1:/", etc.
        Part->Name[0] = (char)('0' + DriveIndex);
        Part->Name[1] = ':';
        Part->Name[2] = (char)('1' + p);
        Part->Name[3] = ':';
        Part->Name[4] = '/';
        Part->Name[5] = '\0';

        Part->Block.Private = Part;
        Part->Block.Read = PartitionRead;
        Part->Block.Write = PartitionWrite;
        Part->Block.Flush = PartitionFlush;

        HddPrintPartition(Part, Disk->Drive.SectorSize);
        PartitionCount++;
    }
}

void HddInit(void)
{
    AtaDrive Found[HddMaxDrives];
    int DmaController;
    int Detected;
    int i;

    HddEntryCount = 0;
    PartitionCount = 0;

    DmaController = HddStartDmaController();
    Detected = AtaDetectAll(Found, HddMaxDrives);
    HddIrqInit(Found, Detected);

    for (i = 0; i < Detected && HddEntryCount < HddMaxDrives; i++) {
        HddEntry *Entry = &Entries[HddEntryCount];

        Entry->Drive = Found[i];
        Entry->Drive.Name[0] = (char)('0' + HddEntryCount);
        Entry->Drive.Name[1] = ':';
        Entry->Drive.Name[2] = '/';
        Entry->Drive.Name[3] = '\0';
        Entry->Block.Private = Entry;
        Entry->Block.Read = HddRead;
        Entry->Block.Write = HddWrite;
        Entry->Block.Flush = HddFlush;

        Entry->DmaAvailable = 0;
        if (DmaController && (Entry->Drive.Flags & AtaDriveDma) != 0 &&
            HddProbeDma(&Entry->Drive) == ErrOk) {
            Entry->DmaAvailable = 1;
            (void)AtaSetIrqMode(Entry->Drive.Channel, 1);
        }

        HddPrintDisk(Entry);
        HddDetectPartitions(HddEntryCount);
        HddEntryCount++;
    }

    if (HddEntryCount == 0)
        KPrintln("no ATA drives found");
}

int HddCount(void)
{
    return HddEntryCount;
}

const BlockDevice *HddGet(int Index)
{
    if (Index < 0 || Index >= HddEntryCount)
        return 0;
    return &Entries[Index].Block;
}

int HddPartitionCount(void)
{
    return PartitionCount;
}

const BlockDevice *HddPartitionGet(int Index)
{
    if (Index < 0 || Index >= PartitionCount)
        return 0;
    return &Partitions[Index].Block;
}

const char *HddPartitionName(int Index)
{
    if (Index < 0 || Index >= PartitionCount)
        return 0;
    return Partitions[Index].Name;
}

static int MatchName(const char *Pattern, const char *Name)
{
    int i = 0;

    if (Pattern == 0 || Name == 0)
        return 0;

    while (Pattern[i] != '\0' && Name[i] != '\0') {
        if (Pattern[i] != Name[i])
            return 0;
        i++;
    }
    if (Pattern[i] == '\0' && Name[i] == '\0')
        return 1;
    // Allow matching without trailing slash: "0:" matches "0:/"
    if (Pattern[i] == '/' && Pattern[i + 1] == '\0' && Name[i] == '\0')
        return 1;
    return 0;
}

const BlockDevice *HddFind(const char *Name)
{
    int i;

    if (Name == 0)
        return 0;

    // Check partitions first (e.g. "0:1:/")
    for (i = 0; i < PartitionCount; i++) {
        if (MatchName(Partitions[i].Name, Name))
            return &Partitions[i].Block;
    }

    // Check physical drives (e.g. "0:/", "1:/")
    for (i = 0; i < HddEntryCount; i++) {
        if (MatchName(Entries[i].Drive.Name, Name))
            return &Entries[i].Block;
    }

    return 0;
}
