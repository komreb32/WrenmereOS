/* SPDX-License-Identifier: BSD-3-Clause */
// Static process/thread pools, reusable PID allocator, kernel-stack slots
// and current-context accessors.
//
// Pools: 64 Process + 128 Thread, zeroed at first use; no heap. PIDs come
// from a 64-bit bitmap (bit i = PID i+1), so ProcessAllocPid hands out the
// smallest free PID >= 1 and ProcessFreePid recycles it. KernelStackAlloc
// maps 4 zeroed frames at the top of a free 32 KiB slot under
// 0xFFFFC90000000000 with PagingMap (Present+Writable, kernel-only); the
// low 16 KiB of the slot stay unmapped as the overflow guard.
// CurrentAddressSpace() here is the STRONG override of the weak stub in
// kernel/proc/weak.c: the current process space, else the kernel space.
//
// NOTE (AsMapPage/AsKernel): neither exists in addrspace.rs/the ELF, so
// stacks use PageAlloc + PagingMap directly on the live tables. No VMA is
// recorded for them: kernel stacks are global, not per-address-space, and
// the kernel half is shared anyway. If a per-AS VMA ever becomes wanted,
// AsAddVma(AsKernelStorage, base, top, Present|Writable, Module) is the
// call — but that also needs AsKernel() exported first.
#include <lib/stdint.h>
#include <memlayout.h>
#include <percpu.h>
#include <process.h>
#include <sync.h>
#include <kernel/mm/page_alloc.h>
#include <kernel/mm/paging.h>

// ---- Local stand-ins (C23-safe: no <addrspace.h> typedef bool) ----
typedef struct AddressSpace AddressSpace;

// Paging flags for kernel stacks: Present|Writable, kernel-only (no User).
#define StackMapFlags (PagingPresent | PagingWritable)

// ---- Static pools ----
static Process ProcessPool[ProcessMax];
static Thread ThreadPool[ThreadMax];
static uint32_t ProcessUsed[ProcessMax];
static uint32_t ThreadUsed[ThreadMax];
static uint64_t NextThreadId = 1;

// PID i+1 lives in bit i; 1 = taken.
static uint64_t PidBitmap = 0;

// Kernel stack slots: one per thread, bit i = slot i taken.
static uint64_t StackBitmapLow = 0;  // slots 0..63
static uint64_t StackBitmapHigh = 0; // slots 64..127
#if KStackMax > 128u
#error "stack bitmap covers 128 slots only"
#endif

// Kernel address space storage: same layout as Rust AddrSpace
// (Vma[64] of 32 bytes + VmaCount), all zero = empty space.
typedef struct {
    uint64_t Start;
    uint64_t End;
    uint64_t Flags;
    uint32_t Kind;
    uint32_t Pad;
} KernelVma;
typedef struct {
    KernelVma Vmas[64];
    uint32_t VmaCount;
} KernelAddrSpace;

_Static_assert(sizeof(KernelVma) == 32, "Vma mirror must be 32 bytes");

static KernelAddrSpace KernelSpaceStorage;

static void Zero(void *Dst, uint64_t Len)
{
    uint8_t *P = (uint8_t *)Dst;
    uint64_t i;

    for (i = 0; i < Len; i++)
        P[i] = 0;
}

// ---- Processes ----

uint64_t ProcessAllocPid(void)
{
    uint64_t i;

    for (i = 0; i < ProcessMax; i++)
    {
        if ((PidBitmap & (1ULL << i)) == 0)
        {
            PidBitmap |= 1ULL << i;
            return i + 1;
        }
    }
    return 0;
}

void ProcessFreePid(uint64_t Pid)
{
    if (Pid == 0 || Pid > ProcessMax)
        return;
    PidBitmap &= ~(1ULL << (Pid - 1));
}

Process *ProcessAlloc(void)
{
    uint64_t i;
    uint64_t Pid;

    Pid = ProcessAllocPid();
    if (Pid == 0)
        return 0;
    for (i = 0; i < ProcessMax; i++)
    {
        if (!ProcessUsed[i])
        {
            Process *P = &ProcessPool[i];

            ProcessUsed[i] = 1;
            Zero(P, sizeof(*P));
            P->Pid = Pid;
            P->State = ProcessStateActive;
            P->AddressSpace = (AddressSpace *)&KernelSpaceStorage;
            WaitQueueInit(&P->ChildExit);
            return P;
        }
    }
    // No free slot after all: hand the PID back so it stays reusable.
    ProcessFreePid(Pid);
    return 0;
}

void ProcessFree(Process *P)
{
    uint64_t i;

    if (P == 0)
        return;
    ProcessFreePid(P->Pid);
    for (i = 0; i < ProcessMax; i++)
    {
        if (&ProcessPool[i] == P)
        {
            ProcessUsed[i] = 0;
            Zero(P, sizeof(*P));
            return;
        }
    }
}

Process *ProcessAllocKernel(void)
{
    uint64_t i;

    for (i = 0; i < ProcessMax; i++)
    {
        if (!ProcessUsed[i])
        {
            Process *P = &ProcessPool[i];

            ProcessUsed[i] = 1;
            Zero(P, sizeof(*P));
            P->Pid = 0;
            P->State = ProcessStateActive;
            P->Caps = CapAll;
            P->AddressSpace = (AddressSpace *)&KernelSpaceStorage;
            WaitQueueInit(&P->ChildExit);
            P->Name[0] = 'k';
            P->Name[1] = 'e';
            P->Name[2] = 'r';
            P->Name[3] = 'n';
            P->Name[4] = 'e';
            P->Name[5] = 'l';
            P->Name[6] = '\0';
            return P;
        }
    }
    return 0;
}

Process *ProcessByPid(uint64_t Pid)
{
    uint64_t i;

    for (i = 0; i < ProcessMax; i++)
    {
        if (ProcessUsed[i] && ProcessPool[i].Pid == Pid)
            return &ProcessPool[i];
    }
    return 0;
}

Process *ProcessGetByIndex(uint64_t Index)
{
    if (Index >= ProcessMax || !ProcessUsed[Index])
        return 0;
    return &ProcessPool[Index];
}

AddressSpace *AsKernel(void)
{
    return (AddressSpace *)&KernelSpaceStorage;
}

// ---- Threads ----

Thread *ThreadAlloc(void)
{
    uint64_t i;

    for (i = 0; i < ThreadMax; i++)
    {
        if (!ThreadUsed[i])
        {
            Thread *T = &ThreadPool[i];

            ThreadUsed[i] = 1;
            Zero(T, sizeof(*T));
            T->Id = NextThreadId++;
            if (NextThreadId == 0)
                NextThreadId = 1; // never hand out Id 0 (NULL sentinel)
            T->State = ThreadReady;
            return T;
        }
    }
    return 0;
}

void ThreadFree(Thread *T)
{
    uint64_t i;

    if (T == 0)
        return;
    for (i = 0; i < ThreadMax; i++)
    {
        if (&ThreadPool[i] == T)
        {
            ThreadUsed[i] = 0;
            Zero(T, sizeof(*T));
            return;
        }
    }
}


// ---- Kernel stacks ----

// Unwind helper: drop pages [StackBase, StackBase + Mapped) that the
// mapper already installed, returning their frames.
static void StackUnwind(uint64_t StackBase, uint64_t Mapped)
{
    uint64_t Off = Mapped;

    while (Off > 0)
    {
        uint64_t Phys = 0;

        Off -= PageSize;
        if (PagingTranslate(StackBase + Off, &Phys) == 0)
        {
            PagingUnmap(StackBase + Off);
            PageFree(Phys & ~(uint64_t)(PageSize - 1));
        }
    }
}

// Map one zeroed frame at Virt; 0 ok, -1 on alloc/map failure.
static int StackMapOne(uint64_t Virt)
{
    uint64_t Frame = PageAlloc();

    if (Frame == 0)
        return -1;
    // Zero through the direct map (PhysToVirt), not the target: the page
    // is not mapped at Virt yet.
    Zero(PhysToVirt(Frame), PageSize);
    if (PagingMap(Virt, Frame, StackMapFlags) != 0)
    {
        PageFree(Frame);
        return -1;
    }
    return 0;
}

uint64_t KernelStackAlloc(void)
{
    uint64_t i;

    for (i = 0; i < KStackMax; i++)
    {
        uint64_t Taken = (i < 64) ? (StackBitmapLow & (1ULL << i))
                                  : (StackBitmapHigh & (1ULL << (i - 64)));
        uint64_t SlotBase;
        uint64_t StackBase;
        uint64_t Off;

        if (Taken != 0)
            continue;

        SlotBase = KStackBase + i * KStackSlot;
        StackBase = SlotBase + KStackSlot - KStackSize;

        for (Off = 0; Off < KStackSize; Off += PageSize)
        {
            if (StackMapOne(StackBase + Off) != 0)
            {
                StackUnwind(StackBase, Off);
                return 0;
            }
        }
        if (i < 64)
            StackBitmapLow |= 1ULL << i;
        else
            StackBitmapHigh |= 1ULL << (i - 64);
        return SlotBase;
    }
    return 0;
}

void KernelStackFree(uint64_t StackBase)
{
    uint64_t i;
    uint64_t Base;
    uint64_t Off;

    // SlotBase is the low guard page; reject anything unaligned or outside.
    if (StackBase < KStackBase ||
        (StackBase - KStackBase) % KStackSlot != 0)
        return;
    i = (StackBase - KStackBase) / KStackSlot;
    if (i >= KStackMax)
        return;

    Base = StackBase + KStackSlot - KStackSize;
    for (Off = 0; Off < KStackSize; Off += PageSize)
    {
        uint64_t Phys = 0;

        if (PagingTranslate(Base + Off, &Phys) == 0)
        {
            PagingUnmap(Base + Off);
            PageFree(Phys & ~(uint64_t)(PageSize - 1));
        }
    }
    if (i < 64)
        StackBitmapLow &= ~(1ULL << i);
    else
        StackBitmapHigh &= ~(1ULL << (i - 64));
}

// ---- Current context ----

Thread *CurrentThread(void)
{
    struct PerCpu *Cpu = PerCpuGet();

    if (Cpu == 0)
        return 0;
    return (Thread *)(uintptr_t)Cpu->CurrentThread;
}

Thread *ThreadGetByIndex(uint64_t Index)
{
    if (Index >= ThreadMax)
        return 0;
    return &ThreadPool[Index];
}

Process *CurrentProcess(void)
{
    Thread *T = CurrentThread();

    if (T == 0)
        return 0;
    return T->Process;
}

// Strong override of the weak stub: the fault path in pagefault.c calls
// this, so user faults now resolve against the running process space.
AddressSpace *CurrentAddressSpace(void)
{
    Process *P = CurrentProcess();

    if (P != 0 && P->AddressSpace != 0)
        return P->AddressSpace;
    return (AddressSpace *)&KernelSpaceStorage;
}
