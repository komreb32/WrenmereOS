/* SPDX-License-Identifier: BSD-3-Clause */
// Round-robin scheduler interface.
#ifndef PROC_SCHED_H
#define PROC_SCHED_H

#include <lib/stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the scheduler: adopt the current context as the idle thread of
// process 0. The idle thread never enters the ready queue and just halts the
// CPU when there is nothing else to run.
void SchedInit(void);

// Enqueue a thread at the tail of the ready queue.
void SchedAdd(struct Thread *T);

// Pick the next ready thread (or idle). Used by Yield, blockers and exit.
void Schedule(void);

// Yield the CPU to the next thread in the ready queue.
void Yield(void);

// Block the current thread until it is woken.
void BlockCurrent(void);

// Wake a specific thread.
void Wake(struct Thread *T);

// Sleep for the given number of milliseconds.
void SleepCurrentMs(uint32_t Ms);

// Timer interrupt handler: run at IRQ0.
void SchedTick(void);

// Check whether the scheduler is active.
int SchedIsActive(void);

// Sleeper list (implemented in kernel/sync/waitqueue.c).
void SleeperInsert(struct Thread *T);
void SleeperRemove(struct Thread *T);
void SleeperWakeExpired(void);

#ifdef __cplusplus
}
#endif

#endif
