/* SPDX-License-Identifier: BSD-3-Clause */
// PS/2 controller: data and status ports.
#ifndef KERNEL_DRIVERS_PS2_H
#define KERNEL_DRIVERS_PS2_H

#include <lib/stdint.h>

#define Ps2DataPort    0x60
#define Ps2StatusPort  0x64
#define Ps2CommandPort 0x64

uint8_t Ps2ReadStatus(void);
uint8_t Ps2ReadScancode(void);

#endif
