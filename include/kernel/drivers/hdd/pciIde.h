/* SPDX-License-Identifier: BSD-3-Clause */
// PCI IDE bus-master controller: PCI configuration space access through the
// 0xCF8/0xCFC pair (pciIde.c), the bus-master interface registers of the
// controller (command, status and the physical-region descriptor table) and
// the DMA sector transfers built on top of them (ataDma.c).
#ifndef KERNEL_DRIVERS_HDD_PCIIDE_H
#define KERNEL_DRIVERS_HDD_PCIIDE_H

#include <errors.h>
#include <lib/stdint.h>
#include <kernel/drivers/hdd/ata.h>

// --- PCI configuration space (pciIde.c) ---

// Configuration mechanism #1: a dword address port and a dword data port.
#define PciConfigAddressPort 0xCF8u
#define PciConfigDataPort    0xCFCu

// Address port bit 31 enables the access; the low bits select one dword.
#define PciConfigEnable 0x80000000u

// A bus with no device reads back all ones in the vendor-id field.
#define PciVendorNone 0xFFFFu

// Configuration-space register offsets (dword aligned).
#define PciRegVendorId   0x00u
#define PciRegDeviceId   0x02u
#define PciRegCommand    0x04u
#define PciRegStatus     0x06u
#define PciRegRevision   0x08u
#define PciRegProgIf     0x09u
#define PciRegSubClass   0x0Au
#define PciRegClass      0x0Bu
#define PciRegHeaderType 0x0Eu
#define PciRegBar0       0x10u // BAR0..BAR5 occupy 0x10..0x24
#define PciRegBar4       0x20u // the bus-master interface base

// Class code of the IDE controllers we drive: class 0x01 (mass storage),
// subclass 0x01 (IDE). The programming-interface byte reports the bus-master
// capability in its top bit.
#define PciClassStorage  0x01u
#define PciSubClassIde   0x01u
#define PciProgIfBusMaster 0x80u

// Command-register bits we touch.
#define PciCmdIoSpace   0x0001u
#define PciCmdMemSpace  0x0002u
#define PciCmdBusMaster 0x0004u

// A base address register reports its kind in the low bits; the mask keeps
// the aligned base of an I/O region (bits 31:2).
#define PciBarIoSpace 0x00000001u
#define PciBarIoMask  0x0000FFFCu

// Build the dword address for one configuration register.
uint32_t PciConfigAddress(uint8_t Bus, uint8_t Device, uint8_t Function,
                          uint8_t Offset);
// Raw configuration access: read and write whole dwords through 0xCF8/0xCFC.
uint32_t PciConfigReadDword(uint8_t Bus, uint8_t Device, uint8_t Function,
                            uint8_t Offset);
void PciConfigWriteDword(uint8_t Bus, uint8_t Device, uint8_t Function,
                         uint8_t Offset, uint32_t Value);
// Sub-dword readers built on the dword pair.
uint16_t PciConfigReadWord(uint8_t Bus, uint8_t Device, uint8_t Function,
                           uint8_t Offset);
uint8_t PciConfigReadByte(uint8_t Bus, uint8_t Device, uint8_t Function,
                          uint8_t Offset);

// One located IDE controller: its bus position and the physical I/O base of
// its bus-master register block (BAR4).
typedef struct
{
    uint8_t  Bus;
    uint8_t  Device;
    uint8_t  Function;
    uint8_t  ProgIf;        // programming interface (bus-master bit 7)
    uint16_t VendorId;
    uint16_t DeviceId;
    uint32_t BusMasterBase; // physical base of BAR4 (primary +0, secondary +8)
} PciIdeController;

// Bus-master register offsets from the controller base. Each channel owns a
// command, a status and a descriptor-table pointer register.
#define PciIdePrimaryCommand   0x00u
#define PciIdePrimaryStatus    0x02u
#define PciIdePrimaryPrd       0x04u
#define PciIdeSecondaryCommand 0x08u
#define PciIdeSecondaryStatus  0x0Au
#define PciIdeSecondaryPrd     0x0Cu

// Bus-master command-register bits (per channel).
#define PciIdeCmdStart 0x01u // start the transfer
#define PciIdeCmdRead  0x08u // 1 = drive to memory, 0 = memory to drive

// Bus-master status-register bits (per channel); error and interrupt are
// cleared by writing a one back to them.
#define PciIdeStatusActive  0x01u // transfer in progress
#define PciIdeStatusError   0x02u // bus-master error (write 1 to clear)
#define PciIdeStatusIrq     0x04u // interrupt asserted (write 1 to clear)
#define PciIdeStatusDrive0  0x20u // drive 0 is DMA capable
#define PciIdeStatusDrive1  0x40u // drive 1 is DMA capable
#define PciIdeStatusSimplex 0x80u // only one channel may be active

// Locate the first IDE controller (class 0x01, subclass 0x01) on the bus,
// record its position and the physical base of BAR4. Returns ErrOk or ErrIo.
int PciIdeFind(PciIdeController *Controller);
// Set the I/O-space and bus-master bits of the controller's command register
// so its bus-master registers can drive transfers. Returns ErrOk or ErrIo.
int PciIdeEnableBusMaster(const PciIdeController *Controller);

// Bus-master register access for one channel (0 primary, 1 secondary).
uint8_t PciIdeCommandRead(const PciIdeController *Controller, uint8_t Channel);
void PciIdeCommandWrite(const PciIdeController *Controller, uint8_t Channel,
                        uint8_t Value);
uint8_t PciIdeStatusRead(const PciIdeController *Controller, uint8_t Channel);
void PciIdeStatusWrite(const PciIdeController *Controller, uint8_t Channel,
                       uint8_t Value);
uint32_t PciIdePrdRead(const PciIdeController *Controller, uint8_t Channel);
void PciIdePrdWrite(const PciIdeController *Controller, uint8_t Channel,
                    uint32_t Value);

// --- Bus-master DMA sector transfers (ataDma.c) ---

// Move Count sectors between the drive and Buffer using bus-master DMA.
// They validate the range, split the request into 128 KiB chunks, program
// the physical-region descriptor table, issue READ/WRITE DMA (EXT) and wait
// with AtaWaitIrq. On any failure they return the error unfreed so the
// caller can fall back to PIO. ErrUnsupported means the drive does not offer
// DMA or the request needs the 48-bit command set the drive lacks.
int AtaDmaRead(const AtaDrive *Drive, uint64_t Lba, uint64_t Count,
               void *Buffer);
int AtaDmaWrite(const AtaDrive *Drive, uint64_t Lba, uint64_t Count,
                const void *Buffer);

#endif
