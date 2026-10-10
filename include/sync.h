/* SPDX-License-Identifier: BSD-3-Clause */
// IRQ flag save/restore, ticket spinlock and minimal __atomic helpers.
//
// Freestanding: no libc, no <stdbool.h>/<stdatomic.h>. IRQ state is the raw
// RFLAGS value (bit 9 = IF). The spinlock is a fair ticket lock
// { Next, Owner }: takers draw Next, then wait for Owner to catch up, so
// no waiter starves. IrqSave variants are for locks shared with interrupt
// context: they mask IRQs before taking the ticket and restore after.
#ifndef SYNC_H
#define SYNC_H

#include <lib/stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IrqFlagInterrupt (1ULL << 9) // RFLAGS.IF

// Save RFLAGS, then mask IRQs. Returns the flags for IrqRestore().
static inline uint64_t IrqSave(void)
{
    uint64_t Flags;

    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(Flags) : : "memory");
    return Flags;
}

// Restore the IRQ state saved by IrqSave(): re-enables only if IF was set.
static inline void IrqRestore(uint64_t Flags)
{
    // IF was live -> sti; otherwise IRQs stay masked. The branch is on a
    // saved constant, not on live hardware state.
    if (Flags & IrqFlagInterrupt)
        __asm__ volatile ("sti" : : : "memory");
}

// Fair ticket lock. Next = ticket dispenser, Owner = ticket being served.
// Both volatile so the wait loop always re-reads; ordering itself comes
// from the __atomic fetch-add below, not from volatile.
//
// NOTE: the type keeps the requested `SpinLock` name as a struct tag
// (`struct SpinLock`) on purpose: a typedef alias would live in the same
// identifier namespace as the `SpinLock()` function and would not compile
// in C. Use `struct SpinLock` at declaration sites.
struct SpinLock {
    volatile uint32_t Next;
    volatile uint32_t Owner;
};

void SpinInit(struct SpinLock *Lock);
void SpinLock(struct SpinLock *Lock);
void SpinUnlock(struct SpinLock *Lock);
// IRQ-masking pair: returns RFLAGS for SpinUnlockIrqRestore().
uint64_t SpinLockIrqSave(struct SpinLock *Lock);
void SpinUnlockIrqRestore(struct SpinLock *Lock, uint64_t Flags);

// --- Minimal atomics over GCC __atomic (sequentially consistent) ---

static inline uint32_t AtomicLoad(const volatile uint32_t *Ptr)
{
    uint32_t Out;

    __atomic_load(Ptr, &Out, __ATOMIC_SEQ_CST);
    return Out;
}

static inline void AtomicStore(volatile uint32_t *Ptr, uint32_t Value)
{
    __atomic_store(Ptr, &Value, __ATOMIC_SEQ_CST);
}

static inline uint32_t AtomicAdd(volatile uint32_t *Ptr, uint32_t Value)
{
    return __atomic_fetch_add(Ptr, Value, __ATOMIC_SEQ_CST);
}

// 1 when *Ptr held Expected and now holds Desired, 0 otherwise. On failure
// *ExpectedOut receives the actual value (classic compare-exchange shape).
static inline int AtomicCompareExchange(volatile uint32_t *Ptr,
                                        uint32_t Expected,
                                        uint32_t Desired,
                                        uint32_t *ExpectedOut)
{
    uint32_t Want = Expected;

    if (__atomic_compare_exchange(Ptr, &Want, &Desired, 0,
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 1;
    if (ExpectedOut != 0)
        *ExpectedOut = Want;
    return 0;
}

// --- Wait queues ---
// WaitQueue is only a list head. Waiters hang off Thread->Next. Timeouts
// also go on the global sleeper list (Thread->Prev) so a thread can sit on
// both. Every function below takes IrqSave(); from an IRQ handler only
// WakeOne, WakeAll and SemPost are allowed.
typedef struct WaitQueue WaitQueue;

struct WaitQueue {
    struct Thread *Head;
};

void WaitQueueInit(WaitQueue *Queue);
int WaitOn(WaitQueue *Queue, int64_t TimeoutMs);
void WakeOne(WaitQueue *Queue);
void WakeAll(WaitQueue *Queue);
void WaitDetach(struct Thread *T);

// --- Mutex (no spin on the wait queue) ---
typedef struct Mutex Mutex;

struct Mutex {
    struct SpinLock Lock;
    struct WaitQueue Waiters;
    int Held;
};

void MutexInit(Mutex *M);
void MutexLock(Mutex *M);
int MutexTryLock(Mutex *M);
void MutexUnlock(Mutex *M);

// --- Semaphore (counting) ---
typedef struct Semaphore Semaphore;

struct Semaphore {
    struct SpinLock Lock;
    uint32_t Count;
    struct WaitQueue Waiters;
};

void SemInit(Semaphore *S, uint32_t Count);
int SemWait(Semaphore *S, int64_t TimeoutMs);
void SemPost(Semaphore *S);

#ifdef __cplusplus
}
#endif

#endif
