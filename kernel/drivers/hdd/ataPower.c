/* SPDX-License-Identifier: BSD-3-Clause */
// ATA power management: STANDBY, IDLE, SLEEP, standby timer programming and
// drive wake-up. All commands are issued through the PIO task-file helpers
// and return codes from errors.h.
#include <errors.h>
#include <kernel/drivers/hdd/ata.h>
#include <kernel/io/io.h>

// Timeout for power-management commands, which may take a few hundred
// milliseconds to put the drive into the requested state.
#define AtaPowerTimeoutMs 2000u

// Device/head value with the fixed bits (7 and 5) and LBA mode set.
#define AtaPowerDevHeadBase 0xE0u

// Turn a failed wait into an error code. Floating bus or ERR/DF is
// ErrIo and consumes the error register; a wait that ran out or a
// completed command without ERR/DF keeps FailCode instead.
static int AtaPowerStatus(const AtaChannel *Channel, int FailCode)
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

// Power mode values returned by AtaCheckPower, read from IDENTIFY DEVICE
// word 78 (features 0x4F).
#define AtaPowerActive 0x00u
#define AtaPowerStandby 0x01u
#define AtaPowerIdle 0x02u
#define AtaPowerSleep 0x03u

// Minimum and maximum standby timer values accepted by SET FEATURES 0x03
// and by the timeout fields of STANDBY NOW / IDLE IMMEDIATE (wait): the
// value is in units of 5 seconds and the drive accepts 1..255.
#define AtaStandbyTimerMinSec 5u
#define AtaStandbyTimerMaxSec 1275u

// Issue one ATA power command to the selected drive and wait for it to
// complete. The command may take a while, so the caller chooses a
// generous timeout. Returns ErrOk on success, ErrIo on ERR/DF/floating
// bus and ErrTimeout if the drive never clears BSY.
static int AtaPowerCommand(const AtaDrive *Drive, uint8_t Cmd)
{
    const AtaChannel *Channel;

    if (Drive == 0)
        return ErrIo;
    Channel = Drive->Channel;
    if (Channel == 0)
        return ErrIo;
    if ((Drive->Flags & AtaDrivePresent) == 0)
        return ErrUnsupported;

    AtaSelectDrive(Channel, Drive->Slave);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegDevice),
            (uint8_t)(AtaPowerDevHeadBase | AtaDeviceLba));
    AtaDelay400ns(Channel);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegCommand), Cmd);
    AtaDelay400ns(Channel);
    if (AtaWaitNotBusy(Channel, AtaPowerTimeoutMs) != 0)
        return AtaPowerStatus(Channel, ErrTimeout);
    return AtaPowerStatus(Channel, ErrOk);
}

// Put the drive into standby after the requested number of seconds of
// inactivity. The timeout is encoded in the sector-count register exactly
// as the STANDBY NOW (0xE2) and IDLE IMMEDIATE (wait, 0xE3) commands
// expect: units of 5 seconds, value 1..255, i.e. 5..1275 seconds.
int AtaSetStandbyTimer(const AtaDrive *Drive, uint32_t Seconds)
{
    const AtaChannel *Channel;

    if (Drive == 0)
        return ErrIo;
    Channel = Drive->Channel;
    if (Channel == 0)
        return ErrIo;
    if ((Drive->Flags & AtaDrivePresent) == 0)
        return ErrUnsupported;
    if (Seconds < AtaStandbyTimerMinSec || Seconds > AtaStandbyTimerMaxSec)
        return ErrIo;

    // SET FEATURES with feature code 0x03 programs the standby timer; the
    // value goes in the sector-count register, in units of 5 seconds.
    AtaSelectDrive(Channel, Drive->Slave);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegDevice),
            (uint8_t)(AtaPowerDevHeadBase | AtaDeviceLba));
    AtaDelay400ns(Channel);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegFeatures), 0x03u);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegSectorCount),
            (uint8_t)(Seconds / 5));
    OutByte((uint16_t)(Channel->CmdBase + AtaRegCommand),
            (uint8_t)AtaCmdSetFeatures);
    AtaDelay400ns(Channel);
    if (AtaWaitNotBusy(Channel, AtaPowerTimeoutMs) != 0)
        return AtaPowerStatus(Channel, ErrTimeout);
    return AtaPowerStatus(Channel, ErrOk);
}

// Put the drive into standby immediately.
int AtaStandby(const AtaDrive *Drive)
{
    return AtaPowerCommand(Drive, AtaCmdStandbyNow);
}

// Put the drive into standby after the inactivity timeout programmed with
// AtaSetStandbyTimer (or the drive's default). This is STANDBY NOW (0xE2).
int AtaIdle(const AtaDrive *Drive)
{
    return AtaPowerCommand(Drive, AtaCmdStandbyNow);
}

// Put the drive into idle (spin-up ready, heads parked) after the
// programmed inactivity timeout. This is IDLE IMMEDIATE (wait), 0xE3.
int AtaSleep(const AtaDrive *Drive)
{
    return AtaPowerCommand(Drive, AtaCmdIdleImmediateWait);
}

// Read IDENTIFY DEVICE word 78 (features 0x4F) and report the drive's
// current power mode: active, standby, idle or sleep. Returns the power
// mode constant on success, or an errors.h code if the drive is not
// present or the IDENTIFY read fails.
int AtaCheckPower(const AtaDrive *Drive)
{
    const AtaChannel *Channel;
    uint16_t Identify[256];
    uint16_t PowerMode;
    int Rc;

    if (Drive == 0)
        return ErrIo;
    Channel = Drive->Channel;
    if (Channel == 0)
        return ErrIo;
    if ((Drive->Flags & AtaDrivePresent) == 0)
        return ErrUnsupported;

    Rc = AtaPioRead(Drive, 0, 1, Identify);
    if (Rc != ErrOk)
        return Rc;

    PowerMode = Identify[78];
    if (PowerMode == AtaPowerActive)
        return AtaPowerActive;
    if (PowerMode == AtaPowerStandby)
        return AtaPowerStandby;
    if (PowerMode == AtaPowerIdle)
        return AtaPowerIdle;
    if (PowerMode == AtaPowerSleep)
        return AtaPowerSleep;
    return ErrIo; // unknown power mode
}

// Wake a sleeping drive by issuing a single-sector read. A drive in
// standby/idle/sleep ignores nothing but the data port, so a normal read
// command forces it to spin up and return to the active state.
int AtaWakeUp(const AtaDrive *Drive)
{
    const AtaChannel *Channel;
    uint16_t Data;

    if (Drive == 0)
        return ErrIo;
    Channel = Drive->Channel;
    if (Channel == 0)
        return ErrIo;
    if ((Drive->Flags & AtaDrivePresent) == 0)
        return ErrUnsupported;

    AtaSelectDrive(Channel, Drive->Slave);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegDevice),
            (uint8_t)(AtaPowerDevHeadBase | AtaDeviceLba));
    AtaDelay400ns(Channel);
    OutByte((uint16_t)(Channel->CmdBase + AtaRegCommand),
            (uint8_t)AtaCmdReadSectors);
    AtaDelay400ns(Channel);
    if (AtaWaitDrq(Channel, AtaPowerTimeoutMs) != 0)
        return AtaPowerStatus(Channel, ErrTimeout);
    // Consume the sector so the drive completes the wake-up sequence; the
    // data itself is not needed.
    {
        uint32_t Words = Drive->SectorSize / 2;

        if (Words < 2)
            Words = 256;
        InWordBuffer(Channel->CmdBase, &Data, Words);
    }
    (void)Data;
    if (AtaWaitNotBusy(Channel, AtaPowerTimeoutMs) != 0)
        return AtaPowerStatus(Channel, ErrTimeout);
    return AtaPowerStatus(Channel, ErrOk);
}
