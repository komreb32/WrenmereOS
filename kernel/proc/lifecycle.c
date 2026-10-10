/* SPDX-License-Identifier: BSD-3-Clause */
// Process and thread lifecycle: create, exit, wait, kill, reaper and
// user-fault teardown.
//
// ProcessExit marks the current process Zombie, CloseAllHandles (weak
// until the object layer), reparents children to process 1, wakes the
// parent on ChildExit, then ThreadExit(). A dying thread cannot free its
// own stack, so ThreadExit() sets Zombie and Schedule(); the reaper
// reclaims stacks and address spaces. ProcessWait() collects a zombie
// child and frees its Process. ProcessKill sets DyingFlag; teardown
// happens on the next return to ring 3.

#include <errors.h>
#include <lib/stdio.h>
#include <process.h>
#include <proc/sched.h>
#include <sync.h>
#include <syscall.h>
#include <kernel/core/panic.h>

typedef struct {
    uint64_t Start;
    uint64_t End;
    uint64_t Flags;
    uint32_t Kind;
    uint32_t Pad;
} AsVmaMirror;

typedef struct {
    AsVmaMirror Vmas[64];
    uint32_t VmaCount;
} AsMirror;

int32_t AsFreeRange(struct AddressSpace *As, uint64_t Addr, uint64_t Size);

static Thread *Reaper;

static void CopyName(char *Dst, const char *Src)
{
    uint64_t i;

    if (Dst == 0)
        return;
    if (Src == 0) {
        Dst[0] = '\0';
        return;
    }
    for (i = 0; i + 1 < ProcessNameLen && Src[i] != '\0'; i++)
        Dst[i] = Src[i];
    Dst[i] = '\0';
}

static int ProcessHasChildren(Process *Parent)
{
    uint64_t i;
    Process *C;

    for (i = 0; i < ProcessMax; i++) {
        C = ProcessGetByIndex(i);
        if (C != 0 && C->ParentPid == Parent->Pid && C != Parent)
            return 1;
    }
    return 0;
}

static uint64_t ProcessLiveThreads(Process *P)
{
    uint64_t i;
    uint64_t N = 0;
    Thread *T;

    for (i = 0; i < ThreadMax; i++) {
        T = ThreadGetByIndex(i);
        if (T == 0 || T->Process != P)
            continue;
        if (T->Id != 0 && T->State != ThreadZombie)
            N++;
    }
    return N;
}

static void ReleaseAddressSpace(Process *P)
{
    AsMirror *As;
    uint32_t i;
    uint32_t Count;
    AsVmaMirror Copy[64];

    if (P == 0 || P->AddressSpace == 0 || P->AddressSpace == AsKernel())
        return;

    As = (AsMirror *)(uintptr_t)P->AddressSpace;
    Count = As->VmaCount;
    if (Count > 64)
        Count = 64;
    for (i = 0; i < Count; i++)
        Copy[i] = As->Vmas[i];
    for (i = 0; i < Count; i++) {
        if (Copy[i].End > Copy[i].Start)
            AsFreeRange(P->AddressSpace, Copy[i].Start,
                        Copy[i].End - Copy[i].Start);
    }
    P->AddressSpace = 0;
}

static void FreeThreadResources(Thread *T)
{
    Process *P;

    if (T == 0)
        return;

    P = T->Process;
    if (T->KernelStackBase >= KStackBase)
        KernelStackFree(T->KernelStackBase);
    ThreadFree(T);

    if (P != 0 && P->State == ProcessStateZombie && ProcessLiveThreads(P) == 0)
        ReleaseAddressSpace(P);
}

static void ReapZombies(void)
{
    uint64_t i;
    uint64_t Flags;
    Thread *T;
    Thread *Cur;

    Flags = IrqSave();
    Cur = CurrentThread();
    for (i = 0; i < ThreadMax; i++) {
        T = ThreadGetByIndex(i);
        if (T == 0 || T == Cur || T->Id == 0)
            continue;
        if (T->State == ThreadZombie)
            FreeThreadResources(T);
    }
    IrqRestore(Flags);
}

static void ReaperMain(uint64_t Arg)
{
    (void)Arg;
    for (;;) {
        ReapZombies();
        BlockCurrent();
    }
}

__attribute__((weak)) void CloseAllHandles(Process *P)
{
    (void)P;
}

void LifecycleInit(void)
{
    Process *Init;
    Process *KernelProc;

    KernelProc = ProcessByPid(0);
    Init = ProcessCreate("init", 0, CapAll, AsKernel());
    if (Init == 0)
        Panic("LifecycleInit: cannot create init", 0);

    if (KernelProc == 0)
        KernelProc = CurrentProcess();
    Reaper = ThreadCreateKernel(KernelProc, (uint64_t)(uintptr_t)ReaperMain, 0);
    if (Reaper == 0)
        Panic("LifecycleInit: cannot create reaper", 0);
    SchedAdd(Reaper);
}

Process *ProcessCreate(const char *Name, uint64_t ParentPid, uint32_t Caps,
                       struct AddressSpace *As)
{
    Process *P;

    P = ProcessAlloc();
    if (P == 0)
        return 0;

    P->ParentPid = ParentPid;
    P->Caps = Caps;
    P->State = ProcessStateActive;
    P->DyingFlag = 0;
    P->ExitCode = 0;
    P->MainThread = 0;
    if (As != 0)
        P->AddressSpace = As;
    else
        P->AddressSpace = AsKernel();
    CopyName(P->Name, Name);
    WaitQueueInit(&P->ChildExit);
    return P;
}

void ThreadExit(void)
{
    Thread *T;

    T = CurrentThread();
    if (T == 0)
        Panic("ThreadExit with no current thread", 0);

    T->State = ThreadZombie;
    if (Reaper != 0 && Reaper != T)
        Wake(Reaper);
    Schedule();
    for (;;)
        __asm__ volatile ("hlt");
}

void ProcessExit(int64_t Code)
{
    Process *P;
    Process *Parent;
    Process *Child;
    Thread *T;
    uint64_t i;

    P = CurrentProcess();
    T = CurrentThread();
    if (P == 0)
        Panic("ProcessExit with no process", 0);

    P->ExitCode = Code;
    P->State = ProcessStateZombie;
    P->DyingFlag = 0;

    CloseAllHandles(P);

    for (i = 0; i < ProcessMax; i++) {
        Child = ProcessGetByIndex(i);
        if (Child != 0 && Child != P && Child->ParentPid == P->Pid)
            Child->ParentPid = 1;
    }

    Parent = ProcessByPid(P->ParentPid);
    if (Parent != 0)
        WakeOne(&Parent->ChildExit);

    if (T != 0)
        T->ExitCode = Code;
    ThreadExit();
}

static Process *FindZombieChild(Process *Parent, int64_t Pid)
{
    uint64_t i;
    Process *C;

    for (i = 0; i < ProcessMax; i++) {
        C = ProcessGetByIndex(i);
        if (C == 0 || C == Parent || C->ParentPid != Parent->Pid)
            continue;
        if (C->State != ProcessStateZombie)
            continue;
        if (Pid < 0 || (uint64_t)Pid == C->Pid)
            return C;
    }
    return 0;
}

static void CollectZombie(Process *Z)
{
    uint64_t i;
    uint64_t Flags;
    Thread *T;

    Flags = IrqSave();
    for (i = 0; i < ThreadMax; i++) {
        T = ThreadGetByIndex(i);
        if (T == 0 || T->Process != Z || T->Id == 0)
            continue;
        if (T == CurrentThread())
            continue;
        if (T->KernelStackBase >= KStackBase)
            KernelStackFree(T->KernelStackBase);
        ThreadFree(T);
    }
    ReleaseAddressSpace(Z);
    ProcessFree(Z);
    IrqRestore(Flags);
}

int64_t ProcessWait(int64_t Pid, int64_t *Status, int64_t TimeoutMs)
{
    Process *Self;
    Process *Z;
    int Rc;

    Self = CurrentProcess();
    if (Self == 0)
        return ErrNoProcess;

    if (!ProcessHasChildren(Self))
        return ErrNoProcess;

    for (;;) {
        Z = FindZombieChild(Self, Pid);
        if (Z != 0) {
            if (Status != 0)
                *Status = Z->ExitCode;
            Pid = (int64_t)Z->Pid;
            CollectZombie(Z);
            return Pid;
        }
        Rc = WaitOn(&Self->ChildExit, TimeoutMs);
        if (Rc == ErrTimeout)
            return ErrTimeout;
        if (!ProcessHasChildren(Self))
            return ErrNoProcess;
        if (TimeoutMs >= 0)
            TimeoutMs = 0;
    }
}

int ProcessKill(uint64_t Pid, int64_t Code)
{
    Process *P;
    Thread *T;
    uint64_t i;

    P = ProcessByPid(Pid);
    if (P == 0 || P->State == ProcessStateFree)
        return ErrNoProcess;

    P->DyingFlag = 1;
    P->ExitCode = Code;

    for (i = 0; i < ThreadMax; i++) {
        T = ThreadGetByIndex(i);
        if (T == 0 || T->Process != P || T->Id == 0)
            continue;
        if (T->State == ThreadBlocked || T->State == ThreadSleeping) {
            SleeperRemove(T);
            WaitDetach(T);
            T->WaitResult = ErrOk;
            Wake(T);
        }
    }
    return ErrOk;
}

void ProcessCheckDying(void)
{
    Process *P;

    P = CurrentProcess();
    if (P != 0 && P->DyingFlag)
        ProcessExit(P->ExitCode);
}

void ProcessFaultCurrent(TrapFrame *Frame, uint64_t Cr2)
{
    Process *P;
    uint64_t Pid;
    uint64_t Rip;

    P = CurrentProcess();
    Pid = (P != 0) ? P->Pid : 0;
    Rip = (Frame != 0) ? Frame->rip : 0;

    KPrint("[IF] pid=");
    KPrintDec(Pid);
    KPrint(" rip=0x");
    KPrintHex(Rip);
    KPrint(" cr2=0x");
    KPrintHex(Cr2);
    KPrintln("");

    ProcessExit(-1);
}
