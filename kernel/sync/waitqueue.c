/* SPDX-License-Identifier: BSD-3-Clause */
// Wait queues, mutex and counting semaphore.
//
// WaitQueue is only { Head }; list linkage is Thread->Next. The sleeper
// list (timeouts) uses Thread->Prev so a waiter can sit on both at once.
// Every public function runs under IrqSave(). From an IRQ handler only
// WakeOne, WakeAll and SemPost are allowed: they never block.

#include <errors.h>
#include <process.h>
#include <proc/sched.h>
#include <sync.h>
#include <kernel/core/timer.h>

static Thread *SleeperList;

static void WaitUnlink(WaitQueue *Queue, Thread *T)
{
    Thread **P;
    Thread *Cand;

    if (Queue == 0 || T == 0)
        return;

    P = &Queue->Head;
    for (;;) {
        Cand = *P;
        if (Cand == 0)
            return;
        if (Cand == T) {
            *P = T->Next;
            T->Next = 0;
            if (T->WaitingOn == Queue)
                T->WaitingOn = 0;
            return;
        }
        P = &Cand->Next;
    }
}

static void WaitEnqueue(WaitQueue *Queue, Thread *T)
{
    Thread *Last;

    T->Next = 0;
    T->WaitingOn = Queue;
    if (Queue->Head == 0) {
        Queue->Head = T;
        return;
    }
    Last = Queue->Head;
    while (Last->Next != 0)
        Last = Last->Next;
    Last->Next = T;
}

void SleeperRemove(Thread *T)
{
    Thread **P;
    Thread *Cand;

    if (T == 0)
        return;

    P = &SleeperList;
    for (;;) {
        Cand = *P;
        if (Cand == 0)
            return;
        if (Cand == T) {
            *P = T->Prev;
            T->Prev = 0;
            T->WakeTick = 0;
            return;
        }
        P = &Cand->Prev;
    }
}

void SleeperInsert(Thread *T)
{
    Thread **P;
    Thread *Cand;

    if (T == 0)
        return;

    SleeperRemove(T);
    P = &SleeperList;
    for (;;) {
        Cand = *P;
        if (Cand == 0 || Cand->WakeTick >= T->WakeTick)
            break;
        P = &Cand->Prev;
    }
    T->Prev = Cand;
    *P = T;
}

void SleeperWakeExpired(void)
{
    uint64_t Now;
    Thread *Head;

    Now = TimerGetTicks();
    for (;;) {
        Head = SleeperList;
        if (Head == 0 || Head->WakeTick > Now)
            return;

        SleeperRemove(Head);
        if (Head->WaitingOn != 0)
            WaitUnlink(Head->WaitingOn, Head);
        Head->WaitResult = ErrTimeout;
        SchedAdd(Head);
    }
}

static void WakeOneLocked(WaitQueue *Queue)
{
    Thread *Woken;

    Woken = Queue->Head;
    if (Woken == 0)
        return;

    Queue->Head = Woken->Next;
    Woken->Next = 0;
    Woken->WaitingOn = 0;
    Woken->WaitResult = ErrOk;
    SleeperRemove(Woken);
    SchedAdd(Woken);
}

void WaitDetach(Thread *T)
{
    if (T == 0 || T->WaitingOn == 0)
        return;
    WaitUnlink(T->WaitingOn, T);
}

void WaitQueueInit(WaitQueue *Queue)
{
    if (Queue == 0)
        return;
    Queue->Head = 0;
}

int WaitOn(WaitQueue *Queue, int64_t TimeoutMs)
{
    uint64_t Flags;
    Thread *T;
    uint32_t Hz;

    if (Queue == 0)
        return ErrBadHandle;

    T = CurrentThread();
    if (T == 0)
        return ErrNoProcess;

    Flags = IrqSave();

    WaitEnqueue(Queue, T);
    T->WaitResult = ErrOk;
    if (TimeoutMs >= 0) {
        Hz = TimerGetTicksPerSecond();
        if (Hz == 0)
            Hz = 100;
        T->WakeTick = TimerGetTicks()
                    + (uint64_t)TimeoutMs * (uint64_t)Hz / 1000ULL;
        T->WaitResult = ErrTimeout;
        SleeperInsert(T);
    }

    T->State = ThreadBlocked;
    BlockCurrent();

    WaitUnlink(Queue, T);
    SleeperRemove(T);
    T->WaitingOn = 0;

    IrqRestore(Flags);
    return (int)T->WaitResult;
}

void WakeOne(WaitQueue *Queue)
{
    uint64_t Flags;

    if (Queue == 0)
        return;

    Flags = IrqSave();
    WakeOneLocked(Queue);
    IrqRestore(Flags);
}

void WakeAll(WaitQueue *Queue)
{
    uint64_t Flags;

    if (Queue == 0)
        return;

    Flags = IrqSave();
    while (Queue->Head != 0)
        WakeOneLocked(Queue);
    IrqRestore(Flags);
}

void MutexInit(Mutex *M)
{
    if (M == 0)
        return;
    SpinInit(&M->Lock);
    WaitQueueInit(&M->Waiters);
    M->Held = 0;
}

void MutexLock(Mutex *M)
{
    uint64_t Flags;
    Thread *T;

    if (M == 0)
        return;

    T = CurrentThread();
    for (;;) {
        Flags = IrqSave();
        if (!M->Held) {
            M->Held = 1;
            IrqRestore(Flags);
            return;
        }
        if (T == 0) {
            IrqRestore(Flags);
            return;
        }
        // Enqueue while IRQs are off so Unlock cannot drop the wakeup.
        WaitEnqueue(&M->Waiters, T);
        T->WaitResult = ErrOk;
        T->State = ThreadBlocked;
        BlockCurrent();
        WaitUnlink(&M->Waiters, T);
        IrqRestore(Flags);
    }
}

int MutexTryLock(Mutex *M)
{
    uint64_t Flags;
    int Acquired;

    if (M == 0)
        return 0;

    Flags = IrqSave();
    Acquired = !M->Held;
    if (Acquired)
        M->Held = 1;
    IrqRestore(Flags);
    return Acquired;
}

void MutexUnlock(Mutex *M)
{
    uint64_t Flags;

    if (M == 0)
        return;

    Flags = IrqSave();
    if (M->Held) {
        M->Held = 0;
        WakeOneLocked(&M->Waiters);
    }
    IrqRestore(Flags);
}

void SemInit(Semaphore *S, uint32_t Count)
{
    if (S == 0)
        return;
    SpinInit(&S->Lock);
    WaitQueueInit(&S->Waiters);
    S->Count = Count;
}

int SemWait(Semaphore *S, int64_t TimeoutMs)
{
    uint64_t Flags;
    Thread *T;
    uint32_t Hz;

    if (S == 0)
        return ErrBadHandle;

    T = CurrentThread();
    Flags = IrqSave();

    if (S->Count > 0) {
        S->Count--;
        IrqRestore(Flags);
        return ErrOk;
    }

    if (T == 0) {
        IrqRestore(Flags);
        return ErrNoProcess;
    }

    WaitEnqueue(&S->Waiters, T);
    T->WaitResult = ErrOk;
    if (TimeoutMs >= 0) {
        Hz = TimerGetTicksPerSecond();
        if (Hz == 0)
            Hz = 100;
        T->WakeTick = TimerGetTicks()
                    + (uint64_t)TimeoutMs * (uint64_t)Hz / 1000ULL;
        T->WaitResult = ErrTimeout;
        SleeperInsert(T);
    }

    T->State = ThreadBlocked;
    BlockCurrent();

    WaitUnlink(&S->Waiters, T);
    SleeperRemove(T);
    T->WaitingOn = 0;

    IrqRestore(Flags);
    return (int)T->WaitResult;
}

void SemPost(Semaphore *S)
{
    uint64_t Flags;

    if (S == 0)
        return;

    Flags = IrqSave();
    if (S->Waiters.Head != 0)
        WakeOneLocked(&S->Waiters);
    else
        S->Count++;
    IrqRestore(Flags);
}
