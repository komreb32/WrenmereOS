/* SPDX-License-Identifier: BSD-3-Clause */
// PIT channel 0 timer: ticks, uptime and sleeps.
#ifndef KERNEL_CORE_TIMER_H
#define KERNEL_CORE_TIMER_H

#include <lib/stdint.h>

void TimerInit(int hz);
int TimerStart(int hz);
uint64_t TimerGetTicks(void);
uint64_t TimerUptimeMs(void);
void TimerSleepMs(uint32_t ms);

#endif
