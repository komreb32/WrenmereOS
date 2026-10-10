/* SPDX-License-Identifier: BSD-3-Clause */
// Fair ticket spinlock: SpinInit/SpinLock/SpinUnlock plus the IRQ-masking
// pair for locks shared with interrupt context.
//
// Lock protocol: draw a ticket with an atomic fetch-add on Next, then spin
// until Owner reaches it. Unlock bumps Owner, handing the lock to the next
// ticket in FIFO order. The ticket gap is u32 arithmetic, so 2^32 handovers
// without an unlock would alias — not a real path (that would be a lock
// held across four billion acquisitions).
#include <sync.h>

// Hint to the CPU while spinning: lets a hyperthreaded sibling run instead
// of burning the shared pipeline on a tight read loop.
static inline void CpuRelax(void)
{
    __asm__ volatile ("pause" : : : "memory");
}

void SpinInit(struct SpinLock *Lock)
{
    if (Lock == 0)
        return;
    Lock->Next = 0;
    Lock->Owner = 0;
}

void SpinLock(struct SpinLock *Lock)
{
    // Draw first: the fetch-add is the linearization point, so two CPUs
    // can never hold the same ticket even if both spin below at once.
    uint32_t Ticket = __atomic_fetch_add(&Lock->Next, 1u, __ATOMIC_ACQUIRE);

    while (__atomic_load_n(&Lock->Owner, __ATOMIC_ACQUIRE) != Ticket)
        CpuRelax();
}

void SpinUnlock(struct SpinLock *Lock)
{
    // Hand over to the next ticket. Release ordering pairs with the
    // acquire load above: everything inside the critical section is
    // visible to whoever observes the new Owner.
    __atomic_store_n(&Lock->Owner, Lock->Owner + 1u, __ATOMIC_RELEASE);
}

uint64_t SpinLockIrqSave(struct SpinLock *Lock)
{
    uint64_t Flags = IrqSave();

    SpinLock(Lock);
    return Flags;
}

void SpinUnlockIrqRestore(struct SpinLock *Lock, uint64_t Flags)
{
    SpinUnlock(Lock);
    IrqRestore(Flags);
}
