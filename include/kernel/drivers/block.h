/* SPDX-License-Identifier: BSD-3-Clause */
// Generic block device view: one function table plus the driver's private
// state. Callers pass LBA-addressed sector runs; Read, Write and Flush
// return 0 (ErrOk) on success and ErrIo/ErrTimeout from errors.h on
// failure. Sectors are the device's own sector size long.
#ifndef KERNEL_DRIVERS_BLOCK_H
#define KERNEL_DRIVERS_BLOCK_H

#include <lib/stdint.h>

typedef struct BlockDevice BlockDevice;

typedef int (*BlockReadFn)(BlockDevice *Device, uint64_t Lba,
                           uint64_t Count, void *Buffer);
typedef int (*BlockWriteFn)(BlockDevice *Device, uint64_t Lba,
                            uint64_t Count, const void *Buffer);
typedef int (*BlockFlushFn)(BlockDevice *Device);

struct BlockDevice
{
    void *Private; // driver state: the ATA adapters cast it to AtaDrive *
    BlockReadFn Read;
    BlockWriteFn Write;
    BlockFlushFn Flush;
};

#endif