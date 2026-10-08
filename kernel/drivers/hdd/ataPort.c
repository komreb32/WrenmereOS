/* SPDX-License-Identifier: BSD-3-Clause */
// ATA channel port access: status reads, 400 ns delays, drive selection,
// BSY/DRQ polling with timeouts and the SRST soft reset sequence.
#include <kernel/core/timer.h>
#include <kernel/drivers/hdd/ata.h>
#include <kernel/io/io.h>

// Generous reset deadline in milliseconds; a device that never clears
// BSY is reported as -1 instead of hanging the caller.
#define AtaResetTimeoutMs 5000u

uint8_t AtaReadStatus(const AtaChannel *Channel)
{
    if (Channel == 0)
        return 0xFF;
    return InByte((uint16_t)(Channel->CmdBase + AtaRegStatus));
}

// Alt status shares the control port with device control. Reading it has
// no side effects: pending interrupts are not acknowledged.
uint8_t AtaReadAltStatus(const AtaChannel *Channel)
{
    if (Channel == 0)
        return 0xFF;
    return InByte((uint16_t)(Channel->CtlBase + AtaRegAltStatus));
}

// The ATA spec allows the host to wait at least 400 ns after certain
// register accesses (drive select, command write). Four alt-status reads
// are the canonical no-side-effect delay.
void AtaDelay400ns(const AtaChannel *Channel)
{
    (void)AtaReadAltStatus(Channel);
    (void)AtaReadAltStatus(Channel);
    (void)AtaReadAltStatus(Channel);
    (void)AtaReadAltStatus(Channel);
}

// Program the device/head register and give the drive its 400 ns settle
// time before the next command write.
void AtaSelectDrive(const AtaChannel *Channel, uint8_t Slave)
{
    if (Channel == 0)
        return;
    OutByte((uint16_t)(Channel->CmdBase + AtaRegDevice),
            (uint8_t)(AtaDeviceLba | (Slave ? AtaDeviceSlave : 0)));
    AtaDelay400ns(Channel);
}

int AtaWaitNotBusy(const AtaChannel *Channel, uint32_t TimeoutMs)
{
    uint64_t deadline;

    if (Channel == 0)
        return -1;
    deadline = TimerUptimeMs() + TimeoutMs;
    for (;;) {
        uint8_t status = AtaReadStatus(Channel);

        if (status == 0xFF)
            return -1; // floating bus: nothing is connected
        if ((status & AtaStatusBsy) == 0)
            return 0;
        if (TimerUptimeMs() >= deadline)
            return -1;
    }
}

// Wait until the device posts DRQ for a data transfer. ERR or DF fails
// the wait; BSY keeps it waiting.
int AtaWaitDrq(const AtaChannel *Channel, uint32_t TimeoutMs)
{
    uint64_t deadline;

    if (Channel == 0)
        return -1;
    deadline = TimerUptimeMs() + TimeoutMs;
    for (;;) {
        uint8_t status = AtaReadStatus(Channel);

        if (status == 0xFF)
            return -1;
        if ((status & AtaStatusBsy) != 0) {
            // still working
        } else if (status & (AtaStatusErr | AtaStatusDfy)) {
            return -1;
        } else if (status & AtaStatusDrq) {
            return 0;
        } else {
            // Neither busy, DRQ nor error: the command finished without
            // transferring data. That is not the DRQ the caller wanted.
            return -1;
        }
        if (TimerUptimeMs() >= deadline)
            return -1;
    }
}

// Assert SRST through the control port, release it, then wait for the
// device to finish its reset sequence (BSY clears). The caller should
// follow with AtaSelectDrive and IDENTIFY.
int AtaSoftReset(const AtaChannel *Channel)
{
    if (Channel == 0)
        return -1;
    OutByte((uint16_t)(Channel->CtlBase + AtaRegDeviceCtl),
            (uint8_t)(AtaCtlNien | AtaCtlSrst));
    AtaDelay400ns(Channel);
    OutByte((uint16_t)(Channel->CtlBase + AtaRegDeviceCtl), AtaCtlNien);
    AtaDelay400ns(Channel);
    return AtaWaitNotBusy(Channel, AtaResetTimeoutMs);
}
