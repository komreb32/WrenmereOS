/* SPDX-License-Identifier: BSD-3-Clause */
// Hard-disk layer: enumerate the ATA drives, bring up their interrupt lines,
// pick bus-master DMA where the PCI IDE controller and the drive allow it and
// expose each drive as a BlockDevice named hd0..hd3. Reads and writes use DMA
// when it is available and fall back to PIO when it is not or when a DMA
// transfer fails.
#ifndef KERNEL_DRIVERS_HDD_HDD_H
#define KERNEL_DRIVERS_HDD_HDD_H

#include <lib/stdint.h>
#include <kernel/drivers/block.h>
#include <kernel/drivers/hdd/ata.h>

// One entry per ATA position: primary/secondary channel, master/slave.
#define HddMaxDrives 4

// Enumerate the ATA drives on both channels, register the channel interrupts,
// test bus-master DMA and register each drive as a BlockDevice ("0:/", "1:/")
// and each MBR partition ("0:1:/", "0:2:/") with DMA-or-PIO Read/Write/Flush.
// Prints detected drives and partitions. Call it after paging and memory are
// up and before the keyboard loop enables interrupts.
void HddInit(void);

// Number of registered drives and a read-only view of one of them by index.
int HddCount(void);
const BlockDevice *HddGet(int Index);

// Partitions: up to 4 primary partitions per drive.
#define HddMaxPartitions 16

// Number of registered partitions and accessors.
int HddPartitionCount(void);
const BlockDevice *HddPartitionGet(int Index);
const char *HddPartitionName(int Index);

// Find a block device by name: physical drive ("0:/", "1:/") or
// partition ("0:1:/", "0:2:/", "1:1:/").
const BlockDevice *HddFind(const char *Name);

// Probe the master and slave of both ATA channels with IDENTIFY DEVICE and
// fill Out[0..N) with the drives that answered. Returns the number found, up
// to Max.
int AtaDetectAll(AtaDrive *Out, int Max);

#endif
