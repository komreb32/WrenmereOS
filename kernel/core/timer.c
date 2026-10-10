/* SPDX-License-Identifier: BSD-3-Clause */
#include <lib/stdint.h>
#include <kernel/core/timer.h>
#include <kernel/idt/idt.h>
#include <kernel/io/io.h>
#include <proc/sched.h>

#define PitOscFreq         1193182u
#define PitChannel0Data    0x40
#define PitCommandRegister 0x43
#define PitCommandSquare   0x36
#define PitMaxDivisor      65535u

static volatile uint64_t TickCount = 0;
static uint32_t TicksPerSecond = 0;

static int IrqEnabled(void)
{
    uint64_t flags;

    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    return (flags & 0x200) != 0;
}

static void TimerCallback(InterruptFrame *frame)
{
    (void)frame;

    TickCount++;
    if (SchedIsActive())
        SchedTick();
}

void TimerInit(int hz)
{
    if (hz <= 0)
        return;

    // 16-bit reload register, clamp the divisor to its range
    uint32_t divisor = PitOscFreq / (uint32_t)hz;

    if (divisor == 0)
        divisor = 1;
    else if (divisor > PitMaxDivisor)
        divisor = PitMaxDivisor;

    OutByte(PitCommandRegister, PitCommandSquare);
    IoWait();
    OutByte(PitChannel0Data, (uint8_t)(divisor & 0xFF));
    IoWait();
    OutByte(PitChannel0Data, (uint8_t)((divisor >> 8) & 0xFF));

    // the real rate, not the requested one
    TicksPerSecond = PitOscFreq / divisor;
    IrqRegister(0, TimerCallback);
}

int TimerStart(int hz)
{
    if (hz <= 0)
        return -1;

    TimerInit(hz);
    return (TicksPerSecond != 0) ? 0 : -1;
}

uint64_t TimerGetTicks(void)
{
    return TickCount;
}

uint32_t TimerGetTicksPerSecond(void)
{
    return TicksPerSecond;
}

uint64_t TimerUptimeMs(void)
{
    uint64_t Frequency;
    uint64_t Seconds;
    uint64_t Remainder;
    uint64_t Milliseconds;
    uint64_t Fraction;

    if (TicksPerSecond == 0)
        return 0;

    Frequency = TicksPerSecond;
    Seconds = TickCount / Frequency;
    Remainder = TickCount % Frequency;

    if (Seconds > UINT64_MAX / 1000ULL)
        return UINT64_MAX;

    Milliseconds = Seconds * 1000ULL;
    Fraction = Remainder * 1000ULL / Frequency;
    if (Fraction > UINT64_MAX - Milliseconds)
        return UINT64_MAX;

    return Milliseconds + Fraction;
}

// sleeps for at least ms milliseconds, halting until the timer wakes us
void TimerSleepMs(uint32_t ms)
{
    if (ms == 0)
        return;

    if (SchedIsActive()) {
        SleepCurrentMs(ms);
        return;
    }

    if (TicksPerSecond == 0)
        return;

    if (!IrqEnabled())
        return; // no timer irq would arrive, so we would halt forever

    uint32_t secs = ms / 1000u;
    uint32_t rem  = ms % 1000u;
    uint32_t deadline = TickCount + secs * TicksPerSecond
                      + (rem * TicksPerSecond + 999u) / 1000u; // round up

    while ((int32_t)(TickCount - deadline) < 0)
        __asm__ volatile ("hlt");
}
