/* SPDX-License-Identifier: BSD-3-Clause */
#include <kernel/drivers/ps2.h>
#include <kernel/io/io.h>

uint8_t Ps2ReadStatus(void)
{
    return InByte(Ps2StatusPort);
}

uint8_t Ps2ReadScancode(void)
{
    // wait until a byte sits in the output buffer
    while (!(Ps2ReadStatus() & 0x01))
    {
    }

    return InByte(Ps2DataPort);
}
