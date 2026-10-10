/* SPDX-License-Identifier: BSD-3-Clause */
// Round-robin scheduler: a singly linked ready queue protected by a
// SpinLockIrqSave, and a sleeper list (in waitqueue.c) ordered by WakeTick.
//
// Kernel threads switch with Yield() or by blocking. Idle is never queued:
// Schedule() falls back to it when nothing is ready, and IdleLoop() polls
// the ready queue after each hlt so a timer wakeup can resume sleepers.

#include <lib/stdint.h>
#include <percpu.h>
#include <process.h>
#include <sync.h>
#include <proc/sched.h>
#include <kernel/core/timer.h>
#include <kernel/core/panic.h>
#include <kernel/gdt/gdt.h>

#define TimeSliceTicks 10u

static struct Thread *ReadyHead = 0;
static struct Thread *ReadyTail = 0;
static struct SpinLock ReadyLock;
static volatile int SchedulerActive = 0;
static volatile int NeedResched = 0;
static Thread *IdleThread = 0;

extern char KernelBootStackBottom[];
extern char KernelBootStackTop[];

void ThreadSwitch(Thread *From, Thread *To);
void IdleLoop(void);

void SchedAdd(Thread *T)
{
    uint64_t Flags;

    if (T == 0 || T == IdleThread)
        return;

    Flags = SpinLockIrqSave(&ReadyLock);

    T->State = ThreadReady;
    T->Next = 0;
    if (ReadyHead == 0) {
        ReadyHead = T;
        ReadyTail = T;
    } else {
        ReadyTail->Next = T;
        ReadyTail = T;
    }

    SpinUnlockIrqRestore(&ReadyLock, Flags);
}

static Thread *SchedDequeue(void)
{
    uint64_t Flags;
    Thread *T;

    Flags = SpinLockIrqSave(&ReadyLock);

    T = ReadyHead;
    if (T != 0) {
        ReadyHead = T->Next;
        if (ReadyHead == 0)
            ReadyTail = 0;
        T->Next = 0;
    }

    SpinUnlockIrqRestore(&ReadyLock, Flags);
    return T;
}

void SchedInit(void)
{
    Process *KernelProc;
    Thread *Main;

    SpinInit(&ReadyLock);
    ReadyHead = 0;
    ReadyTail = 0;
    NeedResched = 0;

    KernelProc = ProcessAllocKernel();
    if (KernelProc == 0)
        Panic("SchedInit: cannot create process 0", 0);

    Main = ThreadAlloc();
    if (Main == 0)
        Panic("SchedInit: no thread slot for bootstrap", 0);

    Main->Process = KernelProc;
    Main->KernelStackBase = (uint64_t)(uintptr_t)KernelBootStackBottom;
    Main->KernelStackTop = (uint64_t)(uintptr_t)KernelBootStackTop;
    Main->State = ThreadRunning;
    Main->TimeSlice = TimeSliceTicks;
    KernelProc->MainThread = Main;

    TssSetRsp0(Main->KernelStackTop);
    PerCpuSetKernelRsp(Main->KernelStackTop);
    PerCpuGet()->CurrentThread = (uint64_t)(uintptr_t)Main;

    IdleThread = ThreadCreateKernel(KernelProc, (uint64_t)(uintptr_t)IdleLoop, 0);
    if (IdleThread == 0)
        Panic("SchedInit: no idle thread", 0);
    IdleThread->State = ThreadReady;

    SchedulerActive = 1;
    LifecycleInit();
}

void IdleLoop(void)
{
    for (;;) {
        uint64_t Flags;
        Thread *Next;
        Thread *Cur;

        Flags = IrqSave();
        Next = SchedDequeue();
        if (Next != 0) {
            Cur = CurrentThread();
            Next->State = ThreadRunning;
            Next->TimeSlice = TimeSliceTicks;
            ThreadSwitch(Cur, Next);
            IrqRestore(Flags);
            continue;
        }
        IrqRestore(Flags);
        __asm__ volatile ("hlt");
    }
}

void Schedule(void)
{
    uint64_t Flags;
    Thread *Next;
    Thread *Cur;

    Flags = IrqSave();
    Cur = CurrentThread();
    Next = SchedDequeue();
    if (Next == 0)
        Next = IdleThread;

    if (Next == 0) {
        IrqRestore(Flags);
        for (;;)
            __asm__ volatile ("hlt");
    }

    if (Next == Cur) {
        Cur->State = ThreadRunning;
        IrqRestore(Flags);
        return;
    }

    Next->State = ThreadRunning;
    Next->TimeSlice = TimeSliceTicks;
    ThreadSwitch(Cur, Next);
    IrqRestore(Flags);
}

void Yield(void)
{
    Thread *Cur;

    Cur = CurrentThread();
    if (Cur == 0)
        return;

    Cur->State = ThreadReady;
    SchedAdd(Cur);
    Schedule();
}

void BlockCurrent(void)
{
    Thread *Cur;

    Cur = CurrentThread();
    if (Cur == 0)
        return;

    if (Cur->State != ThreadSleeping)
        Cur->State = ThreadBlocked;
    Schedule();
}

void Wake(Thread *T)
{
    if (T == 0)
        return;

    if (T->State == ThreadBlocked || T->State == ThreadSleeping)
        SchedAdd(T);
}

void SleepCurrentMs(uint32_t Ms)
{
    Thread *T;
    uint64_t Flags;
    uint32_t Hz;

    T = CurrentThread();
    if (T == 0 || Ms == 0)
        return;

    Flags = IrqSave();
    Hz = TimerGetTicksPerSecond();
    if (Hz == 0)
        Hz = 100;
    T->WakeTick = TimerGetTicks() + (uint64_t)Ms * (uint64_t)Hz / 1000ULL;
    T->WaitResult = 0;
    T->State = ThreadSleeping;
    SleeperInsert(T);
    BlockCurrent();
    SleeperRemove(T);
    IrqRestore(Flags);
}

void SchedTick(void)
{
    Thread *Cur;

    if (!SchedulerActive)
        return;

    SleeperWakeExpired();

    Cur = CurrentThread();
    if (Cur != 0 && Cur->TimeSlice > 0)
        Cur->TimeSlice--;

    if (Cur != 0 && Cur->TimeSlice == 0 && ReadyHead != 0)
        NeedResched = 1;
}

int SchedIsActive(void)
{
    return SchedulerActive;
}
