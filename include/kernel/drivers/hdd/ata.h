/* SPDX-License-Identifier: BSD-3-Clause */
// ATA PIO hard disk: channel layout, IDENTIFY drive data and port-level
// helpers (ataPort.c). Sector I/O sits on top of these helpers.
#ifndef KERNEL_DRIVERS_HDD_ATA_H
#define KERNEL_DRIVERS_HDD_ATA_H

#include <lib/stdint.h>
#include <kernel/drivers/block.h>

// Classic AT channel layout (primary and secondary).
#define AtaPrimaryCmdBase    0x1F0u
#define AtaPrimaryCtlBase    0x3F6u
#define AtaPrimaryIrq        14u
#define AtaSecondaryCmdBase  0x170u
#define AtaSecondaryCtlBase  0x376u
#define AtaSecondaryIrq      15u

// Register offsets from the command block base.
#define AtaRegData        0x00u // 16-bit, read or write
#define AtaRegError       0x01u // read
#define AtaRegFeatures    0x01u // write
#define AtaRegSectorCount 0x02u
#define AtaRegLba0        0x03u
#define AtaRegLba1        0x04u
#define AtaRegLba2        0x05u
#define AtaRegDevice      0x06u
#define AtaRegCommand     0x07u // write
#define AtaRegStatus      0x07u // read, clears a pending interrupt

// Offsets from the control block base (alt status and device control
// share one port; only one of the two directions is used per access).
#define AtaRegAltStatus 0x00u // read, never clears interrupts
#define AtaRegDeviceCtl 0x00u // write

// Status register bits.
#define AtaStatusErr 0x01u
#define AtaStatusIdx 0x02u
#define AtaStatusCorr 0x04u
#define AtaStatusDrq 0x08u // data ready between the command block and the host
#define AtaStatusDsc 0x10u
#define AtaStatusDfy 0x20u // device fault
#define AtaStatusRdy 0x40u
#define AtaStatusBsy 0x80u

// Device control bits (control block base, write only).
#define AtaCtlNien 0x02u // mask IRQ14/15: this driver polls
#define AtaCtlSrst 0x04u // software reset

// Device/head register bits.
#define AtaDeviceSlave 0x10u // 0 selects master, 1 selects slave
#define AtaDeviceLba   0x40u

// Commands used by the PIO driver.
#define AtaCmdIdentify        0xECu
#define AtaCmdReadSectors     0x20u // 28-bit LBA
#define AtaCmdReadSectorsExt  0x24u // 48-bit LBA
#define AtaCmdWriteSectors    0x30u
#define AtaCmdWriteSectorsExt 0x34u
#define AtaCmdCacheFlush      0xE7u
#define AtaCmdCacheFlushExt   0xEAu
#define AtaCmdSetFeatures     0xEFu
#define AtaCmdStandbyNow      0xE2u // standby after the programmed timeout
#define AtaCmdIdleImmediateWait 0xE3u // idle (parked heads) after timeout
#define AtaCmdSleep           0xE9u // sleep (spin down)
#define AtaCmdReadSmartData   0xD0u // read SMART data
#define AtaCmdReadSmartThresholds 0xD1u // read SMART thresholds

// AtaDrive flags.
#define AtaDrivePresent (1u << 0) // IDENTIFY succeeded
#define AtaDriveLba48   (1u << 1) // 48-bit LBA command set
#define AtaDriveDma     (1u << 2) // MDMA/UDMA modes available
#define AtaDriveSmart   (1u << 3) // SMART support and enabled

// One IDE channel: the command block, the control block and the IRQ line.
typedef struct
{
    uint16_t CmdBase; // command block base (AtaPrimaryCmdBase ...)
    uint16_t CtlBase; // control block base (AtaPrimaryCtlBase ...)
    uint8_t  Irq;     // ISA IRQ line (14 or 15)
    uint8_t  Number;  // channel index: 0 primary, 1 secondary
} AtaChannel;

// One drive behind a channel, filled from IDENTIFY DEVICE data.
typedef struct
{
    const AtaChannel *Channel;
    uint8_t  Slave;       // 0 master, 1 slave (drive/head bit 4)
    uint16_t Flags;       // AtaDrivePresent | AtaDriveLba48 | ...
    uint8_t  UdmaMode;    // highest selected UDMA mode, 0..5
    uint32_t SectorSize;  // bytes per sector (512 or 4096)
    uint64_t SectorCount; // total addressable sectors
    char     Model[41];   // NUL-terminated IDENTIFY strings
    char     Serial[21];
    char     Firmware[9];
    char     Name[8];     // short console name, e.g. "ata0m"
} AtaDrive;

// Port-level access for one channel (ataPort.c). All waits poll until the
// condition holds or TimeoutMs elapses; they return 0 on success and -1
// on timeout, floating bus (status 0xFF) or a null channel.
uint8_t AtaReadStatus(const AtaChannel *Channel);
uint8_t AtaReadAltStatus(const AtaChannel *Channel);
void AtaDelay400ns(const AtaChannel *Channel);
void AtaSelectDrive(const AtaChannel *Channel, uint8_t Slave);
int AtaWaitNotBusy(const AtaChannel *Channel, uint32_t TimeoutMs);
int AtaWaitDrq(const AtaChannel *Channel, uint32_t TimeoutMs);
int AtaSoftReset(const AtaChannel *Channel);

// PIO sector transfers (ataPio.c): validate the range against
// Drive->SectorCount, split the request into LBA28 (256-sector) or LBA48
// (65536-sector) commands, poll DRQ for every sector and retry the whole
// request three times with AtaSoftReset in between. They return ErrOk,
// ErrIo or ErrTimeout (errors.h); AtaPioWrite finishes with AtaFlushCache.
int AtaPioRead(const AtaDrive *Drive, uint64_t Lba, uint64_t Count,
               void *Buffer);
int AtaPioWrite(const AtaDrive *Drive, uint64_t Lba, uint64_t Count,
                const void *Buffer);
int AtaFlushCache(const AtaDrive *Drive);

// BlockDevice adapters (ataPio.c): Device->Private is the AtaDrive.
int AtaBlockRead(BlockDevice *Device, uint64_t Lba, uint64_t Count,
                 void *Buffer);
int AtaBlockWrite(BlockDevice *Device, uint64_t Lba, uint64_t Count,
                  const void *Buffer);
int AtaBlockFlush(BlockDevice *Device);

// Power management (ataPower.c): all functions wait for BSY, check ERR and
// return codes from errors.h. AtaCheckPower returns AtaPowerActive,
// AtaPowerStandby, AtaPowerIdle or AtaPowerSleep on success.
int AtaStandby(const AtaDrive *Drive);
int AtaIdle(const AtaDrive *Drive);
int AtaSleep(const AtaDrive *Drive);
int AtaCheckPower(const AtaDrive *Drive);
int AtaSetStandbyTimer(const AtaDrive *Drive, uint32_t Seconds);
int AtaWakeUp(const AtaDrive *Drive);

// SMART (ataSmart.c): all functions wait for BSY, check ERR and return
// codes from errors.h. AtaSmartStatus returns AtaSmartStatusOk (0x4F/0xC2)
// or AtaSmartStatusThreshold (0xF4/0x2C) on success.
int AtaSmartEnable(const AtaDrive *Drive);
int AtaSmartDisable(const AtaDrive *Drive);
int AtaSmartStatus(const AtaDrive *Drive);
int AtaSmartReadData(const AtaDrive *Drive, void *Buffer);
int AtaSmartReadThresholds(const AtaDrive *Drive, void *Buffer);
int AtaSmartTemperature(const AtaDrive *Drive, uint8_t *Celsius);
int AtaSmartPowerOnHours(const AtaDrive *Drive, uint32_t *Hours);
int AtaSmartReallocated(const AtaDrive *Drive, uint32_t *Count);
int AtaSmartPending(const AtaDrive *Drive, uint32_t *Count);

#endif
