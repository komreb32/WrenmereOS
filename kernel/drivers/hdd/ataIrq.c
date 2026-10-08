/* SPDX-License-Identifier: BSD-3-Clause */
// ATA interrupt mode: IRQ14/15 registration, the nIEN bit of the device
// control register, an hlt-based completion wait and the polling
// fallback. The handler drains both units of the channel so a shared
// line cannot stay stuck. ataPio.c keeps polling untouched; ataDma.c is
// the intended consumer of AtaWaitIrq.
#include <errors.h>
#include <kernel/core/timer.h>
#include <kernel/drivers/hdd/ata.h>
#include <kernel/idt/idt.h>
#include <kernel/io/io.h>

// The classic PC layout: primary channel on IRQ14, secondary on IRQ15.
#define AtaIrqLinePrimary   14u
#define AtaIrqLineSecondary 15u
#define AtaIrqChannelCount  2u

// Registered channel per line (index 0 primary, 1 secondary): written
// by AtaIrqInit, read by the handler on every interrupt.
static const AtaChannel *IrqChannels[AtaIrqChannelCount];

// Posted by the handler when a real device interrupt was drained and
// consumed by AtaWaitIrq. Volatile: the handler writes it behind the
// waiter's back.
static volatile uint8_t IrqPend[AtaIrqChannelCount];

// 1 while the channel is armed (nIEN clear on both units). Only then can
// IRQ14/15 mean a command completion instead of a ghost edge.
static volatile uint8_t IrqArmed[AtaIrqChannelCount];

// House copy of timer.c's IF probe: the hlt wait only finishes with CPU
// interrupts on, otherwise no wakeup would ever arrive.
static int IrqsEnabled(void)
{
    uint64_t Flags;

    __asm__ volatile ("pushfq; popq %0" : "=r"(Flags));
    return (Flags & 0x200) != 0;
}

// Map a channel to its line index. Only the classic layout is accepted:
// IRQ14/15 with Number matching the line (0 primary, 1 secondary).
static int IrqChannelIndex(const AtaChannel *Channel)
{
    int Idx;

    if (Channel == 0)
        return -1;
    if (Channel->Irq != AtaIrqLinePrimary &&
        Channel->Irq != AtaIrqLineSecondary)
        return -1;
    Idx = (int)(Channel->Irq - AtaIrqLinePrimary);
    if (Channel->Number != (uint8_t)Idx)
        return -1;
    return Idx;
}

// Drain the status register of both units of a channel: that read
// clears the interrupt of whichever unit asserted INTRQ, and the IRQ
// line is shared by the two drives behind the channel, so both have to
// be checked. Returns 0 when the whole channel is a floating bus (both
// reads come back as 0xFF).
static int IrqDrain(const AtaChannel *Channel)
{
    uint8_t Slave;
    int Real = 0;

    for (Slave = 0; Slave < 2; Slave++) {
        uint8_t Status;

        AtaSelectDrive(Channel, Slave);
        Status = AtaReadStatus(Channel);
        if (Status != 0xFF)
            Real = 1;
    }
    return Real;
}

// Handler body for one line. The PIC dispatcher has already filtered
// the spurious IRQ7/IRQ15 ghosts and sends the EOI itself; what is left
// for the device side is to acknowledge the interrupt by reading status
// (which clears INTRQ), to ignore a floating channel, and to wake a
// waiter only while the channel is armed: with nIEN set on both units a
// real device interrupt is impossible, so any line activity then is a
// ghost and must not complete a request.
static void IrqService(int Idx)
{
    const AtaChannel *Channel = IrqChannels[Idx];

    if (Channel == 0)
        return; // line open without a channel: nothing to acknowledge
    if (!IrqDrain(Channel))
        return; // floating bus: spurious, no waiter to wake
    if (!IrqArmed[Idx])
        return; // polling mode: a genuine interrupt cannot be pending
    IrqPend[Idx] = 1;
}

// The IrqHandler signature carries no channel argument, so each line
// gets its own trampoline into IrqService.
static void PrimaryIrqCallback(InterruptFrame *Frame)
{
    (void)Frame;
    IrqService(0);
}

static void SecondaryIrqCallback(InterruptFrame *Frame)
{
    (void)Frame;
    IrqService(1);
}

// Arm or silence a channel through the nIEN bit of the device control
// register (bit 1, active low). That register lives in the task file of
// the *selected* drive, so both units of the shared line get
// programmed. Enabling drains each unit before its nIEN bit drops: a
// leftover interrupt condition would assert INTRQ immediately and would
// false-complete the next AtaWaitIrq. Returns ErrIo for an unknown
// channel or a line that AtaIrqInit never registered.
int AtaSetIrqMode(const AtaChannel *Channel, int Enabled)
{
    int Idx = IrqChannelIndex(Channel);
    uint8_t Control;
    uint8_t Slave;

    if (Idx < 0 || IrqChannels[Idx] == 0)
        return ErrIo;

    Control = Enabled ? 0 : AtaCtlNien; // nIEN is low-true

    for (Slave = 0; Slave < 2; Slave++) {
        AtaSelectDrive(Channel, Slave);
        if (Enabled)
            (void)AtaReadStatus(Channel); // clear a stale interrupt
        OutByte((uint16_t)(Channel->CtlBase + AtaRegDeviceCtl), Control);
        AtaDelay400ns(Channel);
    }

    // Arm last: until this line runs the handler discards every edge,
    // so a ghost during the transition cannot leave a stale wake behind.
    IrqPend[Idx] = 0;
    IrqArmed[Idx] = Enabled ? 1 : 0;
    return ErrOk;
}

// Bring one line up: remember the channel, mask both devices (nIEN set,
// the state AtaSoftReset also leaves behind) and open the PIC line for
// IRQ14/15. Returns ErrIo for a malformed channel or for a line already
// taken by a different channel object.
int AtaIrqInit(const AtaChannel *Channel)
{
    int Idx = IrqChannelIndex(Channel);

    if (Idx < 0)
        return ErrIo;
    if (IrqChannels[Idx] != 0 && IrqChannels[Idx] != Channel)
        return ErrIo;

    IrqChannels[Idx] = Channel;
    IrqPend[Idx] = 0;
    IrqArmed[Idx] = 0;
    (void)AtaSetIrqMode(Channel, 0); // start masked: polling only

    IrqRegister((int)Channel->Irq,
                (Idx == 0) ? PrimaryIrqCallback : SecondaryIrqCallback);
    return ErrOk;
}

// Wait for the completion interrupt of a channel. An armed channel with
// CPU interrupts on sleeps in hlt until the handler raises the flag or
// the deadline passes; every other case (nIEN masked, line never
// registered, caller running with cli) polls BSY instead, so the call
// still finishes without a wakeup source. ErrOk means the condition
// happened (flag raised or BSY clear), not that the command succeeded:
// the caller reads the status register for ERR/DF exactly like the PIO
// layer does. Returns ErrTimeout when the wait runs out and ErrIo for a
// malformed channel or a floating bus.
int AtaWaitIrq(const AtaChannel *Channel, uint32_t TimeoutMs)
{
    int Idx = IrqChannelIndex(Channel);
    uint64_t Deadline;

    if (Idx < 0)
        return ErrIo;

    if (!IrqArmed[Idx] || !IrqsEnabled()) {
        if (AtaWaitNotBusy(Channel, TimeoutMs) != 0) {
            uint8_t Status = AtaReadStatus(Channel);

            return (Status == 0xFF) ? ErrIo : ErrTimeout;
        }
        return ErrOk;
    }

    Deadline = TimerUptimeMs() + TimeoutMs;
    for (;;) {
        if (IrqPend[Idx] != 0) {
            IrqPend[Idx] = 0;
            return ErrOk; // consume the wake-up the handler posted
        }
        if (TimerUptimeMs() >= Deadline)
            return ErrTimeout;
        // sti;hlt as a pair: no interrupt can slip between the flag
        // check and the halt, and any wake-up (timer or ATA) re-runs
        // the loop from the top.
        __asm__ volatile ("sti; hlt" ::: "memory");
    }
}
