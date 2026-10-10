/* SPDX-License-Identifier: BSD-3-Clause */
// #PF handler (vector 14): demand-paging for ring 3, .extable fixups for
// ring 0. Called from IrqDispatch before the generic CpuPanic path.
//
//   CR2        = faulting address.
//   Error bits = P (bit 0, 0 = not-present), W (bit 1, 1 = write),
//                U (bit 2, 1 = user), I (bit 4, 1 = instruction fetch).
//
// Ring 3: try AsHandleFault(CurrentAddressSpace(), cr2, write, true); on
// 0 the page is now backed and iretq retries the faulting instruction.
// Otherwise the fault is real -> ProcessFaultCurrent (weak: Panic today,
// process kill/signal once sched/proc exists).
// Ring 0: a fixup from ExtableFind(frame->rip) resumes the usercopy Raw
// primitive at its clac recovery path; without one it is a kernel bug and
// panics exactly like before, with CR2 shown by Panic for vector 14.
//
// NOTE (firma): se pide PageFaultHandler(TrapFrame*). El dispatcher
// (IrqDispatch) entrega InterruptFrame*; ambos tienen la MISMA cola
// (Vector,Error,rip,cs,rflags,rsp,ss) y este handler solo toca esos
// campos, así que el cast en IrqDispatch es seguro. Los nombres de campo
// difieren en mayúsculas (rip vs Rip): aquí se usan los de TrapFrame.
#include <lib/stdint.h>
#include <syscall.h>
#include <kernel/core/panic.h>
#include <kernel/idt/idt.h>

// Prototipos locales de addrspace (sin incluir <addrspace.h>): ese header
// hace `typedef _Bool bool`, que es error en C23 (bool ya es keyword).
// _Bool es ABI-idéntico al bool de Rust (1 byte), así que la firma coincide.
typedef struct AddressSpace AddressSpace;
typedef _Bool AsBool;
int32_t AsHandleFault(AddressSpace *As, uint64_t Addr, AsBool Write, AsBool User);
// De usercopy.c (sin <usercopy.h>: no se necesita nada más de ese header).
uint64_t ExtableFind(uint64_t Rip);

#define PfErrWrite  (1ULL << 1)
#define PfErrUser   (1ULL << 2)

// Weak process hooks (kernel/proc/weak.c); overridden by sched/proc later.
AddressSpace *CurrentAddressSpace(void);
void ProcessFaultCurrent(TrapFrame *Frame, uint64_t Cr2);

void PageFaultHandler(TrapFrame *Frame)
{
    uint64_t Cr2;
    uint64_t Fixup;

    __asm__ volatile ("mov %%cr2, %0" : "=r"(Cr2));

    // User mode fault: bit 2 of the error code says the access came from
    // ring 3 even when the saved CS is inspected elsewhere.
    if (Frame->error & PfErrUser)
    {
        int Write = (Frame->error & PfErrWrite) != 0;

        if (AsHandleFault(CurrentAddressSpace(), Cr2, Write ? 1 : 0, 1) == 0)
            return;
        ProcessFaultCurrent(Frame, Cr2);
        return; // ProcessFaultCurrent is noreturn today (Panic); keep the tail safe
    }

    // Kernel mode fault: recover through .extable when a usercopy Raw
    // primitive was the faulting instruction.
    Fixup = ExtableFind(Frame->rip);
    if (Fixup != 0)
    {
        Frame->rip = Fixup;
        return;
    }

    // Panic takes InterruptFrame*: same tail layout (vector/error/rip/...),
    // so the cast preserves every field Panic reads (incl. CR2 for vec 14).
    Panic("page fault (#PF)", (const InterruptFrame *)Frame);
}
