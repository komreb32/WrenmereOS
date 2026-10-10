/* SPDX-License-Identifier: BSD-3-Clause */
// Kernel thread switching: ThreadSwitch (FPU save/restore + TSS +
// per-CPU current thread + SwitchContext), ThreadCreateKernel,
// ThreadCreateUser, ThreadTrampoline and ThreadTrampolineUser.
//
// ThreadExit lives in lifecycle.c: a dying thread cannot free its own
// stack, so it becomes a zombie and the reaper reclaims it.

#include <cpu.h>
#include <percpu.h>
#include <process.h>
#include <kernel/core/panic.h>
#include <kernel/gdt/gdt.h>
#include <lib/stdint.h>

void SwitchContext(Thread *From, Thread *To);
void ThreadExit(void);

static void ThreadTrampoline(void);
static void ThreadTrampolineUser(void);

static volatile int FpuInitialized = 0;

static void ThreadInitFpu(Thread *T)
{
    if (FpuInitialized)
        return;
    __asm__ volatile (
        "fninit\n\t"
        "fxsave %0\n\t"
        :
        : "m"(T->FpuState)
        : "memory"
    );
    FpuInitialized = 1;
}

// SwitchContext pops r15,r14,r13,r12,rbx,rbp then rets to Rip.
static void ThreadPrepareFrame(Thread *T, void (*Rip)(void))
{
    uint64_t *Sp;

    Sp = (uint64_t *)(uintptr_t)T->KernelStackTop;
    Sp -= 7;
    Sp[0] = 0;
    Sp[1] = 0;
    Sp[2] = 0;
    Sp[3] = 0;
    Sp[4] = 0;
    Sp[5] = 0;
    Sp[6] = (uint64_t)(uintptr_t)Rip;
    T->SavedRsp = (uint64_t)(uintptr_t)Sp;
}

void ThreadSwitch(Thread *From, Thread *To)
{
    if (To == 0)
        Panic("ThreadSwitch: no destination", 0);
    if (From == 0)
        From = To;

    if (From != To) {
        __asm__ volatile (
            "fxsave %0\n\t"
            "fxrstor %1\n\t"
            :
            : "m"(From->FpuState), "m"(To->FpuState)
            : "memory"
        );
    }

    TssSetRsp0(To->KernelStackTop);
    PerCpuSetKernelRsp(To->KernelStackTop);
    PerCpuGet()->CurrentThread = (uint64_t)(uintptr_t)To;

    // AddressSpace is a VMA list, not a CR3. Kernel threads share the
    // live tables; per-process page tables land here once they exist.
    SwitchContext(From, To);
}

Thread *ThreadCreateKernel(Process *P, uint64_t Entry, uint64_t Arg)
{
    Thread *T;
    uint64_t SlotBase;

    if (P == 0 || Entry == 0)
        return 0;

    T = ThreadAlloc();
    if (T == 0)
        return 0;

    SlotBase = KernelStackAlloc();
    if (SlotBase == 0) {
        ThreadFree(T);
        return 0;
    }

    T->Process = P;
    T->KernelStackBase = SlotBase;
    T->KernelStackTop = SlotBase + KStackSlot;
    T->Entry = Entry;
    T->Arg = Arg;
    T->State = ThreadReady;
    if (P->MainThread == 0)
        P->MainThread = T;

    ThreadPrepareFrame(T, ThreadTrampoline);
    ThreadInitFpu(T);
    return T;
}

Thread *ThreadCreateUser(Process *P, uint64_t UserEntry, uint64_t UserRsp,
                         uint64_t Arg0, uint64_t Arg1)
{
    Thread *T;
    uint64_t SlotBase;

    if (P == 0 || UserEntry == 0)
        return 0;

    T = ThreadAlloc();
    if (T == 0)
        return 0;

    SlotBase = KernelStackAlloc();
    if (SlotBase == 0) {
        ThreadFree(T);
        return 0;
    }

    T->Process = P;
    T->KernelStackBase = SlotBase;
    T->KernelStackTop = SlotBase + KStackSlot;
    T->UserEntry = UserEntry;
    T->UserRsp = UserRsp;
    T->UserArg0 = Arg0;
    T->UserArg1 = Arg1;
    T->State = ThreadReady;
    if (P->MainThread == 0)
        P->MainThread = T;

    ThreadPrepareFrame(T, ThreadTrampolineUser);
    ThreadInitFpu(T);
    return T;
}

void ThreadTrampoline(void)
{
    Thread *T = CurrentThread();
    uint64_t Entry = T->Entry;
    uint64_t Arg = T->Arg;

    ((void (*)(uint64_t))Entry)(Arg);
    ThreadExit();
}

void ThreadTrampolineUser(void)
{
    Thread *T = CurrentThread();

    EnterUserMode(T->UserEntry, T->UserRsp, T->UserArg0, T->UserArg1);
}
