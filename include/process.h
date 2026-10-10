/* SPDX-License-Identifier: BSD-3-Clause */
// Process and thread descriptors: static pools, PID allocator, kernel
// stacks and current-context accessors (docs/abi.md: 64 procs, 128
// threads, 64 handles/proc, 32 KiB stack slots at 0xFFFFC90000000000).
#ifndef PROCESS_H
#define PROCESS_H

#include <lib/stdint.h>
#include <sync.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Process Process;
typedef struct Thread Thread;

// --- Thread states ---
#define ThreadReady    0u
#define ThreadRunning  1u
#define ThreadBlocked  2u
#define ThreadSleeping 3u
#define ThreadZombie   4u

// --- Process states ---
// NOTE: `ProcessState*`, not `Process*`: a bare `ProcessFree` macro would
// collide with the ProcessFree() function below (same identifier space).
#define ProcessStateFree   0u
#define ProcessStateActive 1u
#define ProcessStateZombie 2u

#define ProcessNameLen 32u
#define ProcessHandles 64u

#define ProcessMax 64u
#define ThreadMax  128u

// Kernel stacks (docs/abi.md): 32 KiB slots, 16 KiB stack + 16 KiB guard
// left unmapped below it. Slot i spans [Base + i*Slot, Base + (i+1)*Slot);
// the stack itself is the top 16 KiB, growing down from KStackTop.
// NOTE: macros use the `KStack` prefix (not bare `KernelStack*`) so they
// never collide with the Thread fields below (same identifier space).
#define KStackBase   0xFFFFC90000000000ULL
#define KStackSlot   0x8000ULL  // 32 KiB
#define KStackSize   0x4000ULL  // 16 KiB mapped on top of the slot
#define KStackPages  4u         // 16 KiB / 4 KiB
#define KStackMax    ThreadMax  // one slot per thread

// Capabilities (docs/abi.md, u32 bitmask).
#define CapSpawn   0x01u
#define CapKill    0x02u
#define CapConsole 0x04u
#define CapIoPorts 0x08u
#define CapMapPhys 0x10u
#define CapIrq     0x20u
#define CapSystem  0x40u
#define CapService 0x80u
#define CapAll     0xFFu

struct Thread {
    uint64_t Id;
    uint32_t State; // ThreadReady/Running/Blocked/Sleeping/Zombie
    uint64_t SavedRsp;
    // Requested names are KernelStackBase/Top; kept verbatim (no macro
    // collision now that the region macros use the KStack prefix).
    uint64_t KernelStackBase; // low mapped page of the 16 KiB stack
    uint64_t KernelStackTop;  // one past the top: initial RSP value
    Process *Process;
    uint64_t WakeTick;
    uint64_t TimeSlice;
    int64_t WaitResult;
    int64_t ExitCode;
    uint64_t Entry;     // kernel entry for kernel threads
    uint64_t Arg;       // kernel entry argument
    uint64_t UserEntry; // user RIP for user threads
    uint64_t UserRsp;   // user RSP for user threads
    uint64_t UserArg0;  // rdi at user entry (argc)
    uint64_t UserArg1;  // rsi at user entry (argv)
    uint8_t FpuState[512] __attribute__((aligned(16))); // fxsave/fxrstor area
    Thread *Next;          // ready queue or wait queue (never both)
    Thread *Prev;          // sleeper list
    WaitQueue *WaitingOn;  // wait queue we are blocked on, or NULL
};

struct Process {
    uint64_t Pid;
    uint64_t ParentPid;
    uint32_t State; // ProcessStateFree/Active/Zombie
    char Name[ProcessNameLen];
    struct AddressSpace *AddressSpace;
    uint32_t Caps; // Cap* bitmask
    int64_t ExitCode;
    void *Handles[ProcessHandles]; // NULL for now: no object layer yet
    Thread *MainThread;
    uint64_t Brk;       // program break, page aligned
    uint32_t DyingFlag; // KillProcess requested teardown
    WaitQueue ChildExit;
};

// Opaque address space (same tag as <addrspace.h>; no include here so this
// header stays compilable under C23 without its `typedef _Bool bool`).
struct AddressSpace;

// --- Pools and PID allocator ---
Process *ProcessAlloc(void);
Process *ProcessAllocKernel(void);
void ProcessFree(Process *P);
Thread *ThreadAlloc(void);
void ThreadFree(Thread *T);
Process *ProcessGetByIndex(uint64_t Index);
struct AddressSpace *AsKernel(void);
// Smallest free PID >= 1, 0 when the 64 slots are busy. PIDs are reused
// once their Process is freed.
uint64_t ProcessAllocPid(void);
void ProcessFreePid(uint64_t Pid);
Process *ProcessByPid(uint64_t Pid);

// --- Kernel stacks ---
// Map 4 fresh zeroed frames at the top of a free slot; returns the slot
// base (low guard page) or 0. The guard stays unmapped: overflow faults.
uint64_t KernelStackAlloc(void);
// Unmap the 4 stack pages, return their frames, free the slot. 0 and
// out-of-range bases are ignored.
void KernelStackFree(uint64_t StackBase);

// --- Current context (via PerCpu->CurrentThread, offset 24) ---
Thread *CurrentThread(void);
Process *CurrentProcess(void);
// Accessor for the static thread pool (used by the scheduler/switcher).
Thread *ThreadGetByIndex(uint64_t Index);
// Real override of the weak stub: the current process space, or the
// kernel space when no thread runs yet.
struct AddressSpace *CurrentAddressSpace(void);

Thread *ThreadCreateKernel(Process *P, uint64_t Entry, uint64_t Arg);
Thread *ThreadCreateUser(Process *P, uint64_t UserEntry, uint64_t UserRsp,
                         uint64_t Arg0, uint64_t Arg1);
void ThreadSwitch(Thread *From, Thread *To);

// --- Lifecycle (kernel/proc/lifecycle.c) ---
void LifecycleInit(void);
Process *ProcessCreate(const char *Name, uint64_t ParentPid, uint32_t Caps,
                       struct AddressSpace *As);
void ProcessExit(int64_t Code);
int64_t ProcessWait(int64_t Pid, int64_t *Status, int64_t TimeoutMs);
int ProcessKill(uint64_t Pid, int64_t Code);
void ThreadExit(void);
void ProcessCheckDying(void);
void ProcessFaultCurrent(struct TrapFrame *Frame, uint64_t Cr2);
void CloseAllHandles(Process *P);

#ifdef __cplusplus
}
#endif

#endif
