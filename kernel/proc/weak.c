/* SPDX-License-Identifier: BSD-3-Clause */
// Weak process hooks: placeholders until sched/proc replaces them.
//
// CurrentAddressSpace() returns the kernel address space by default;
// ProcessFaultCurrent() panics on a user fault. Both are weak so the
// future process layer overrides them without touching the caller.
//
// NOTE (firma): se pide "devuelve AsKernel()", pero addrspace.rs no
// exporta ningún AsKernel(): solo el struct AddrSpace. Su layout real es
// Vmas[64] + VmaCount (ver addrspace.rs / include/addrspace.h: Vma). Como
// el tipo C es opaco (sin tamaño), aquí se reserva almacenamiento propio
// con el MISMO tamaño/alineación y se devuelve su puntero: ABI-idéntico a
// un AddrSpace vacío (= sin VMAs; fallos siempre a Panic hasta que proc
// dé un espacio real por proceso).
#include <lib/stdint.h>
#include <syscall.h>
#include <kernel/core/panic.h>
#include <kernel/idt/idt.h>

// Sin <addrspace.h>: su `typedef _Bool bool` es error en C23. Declaración
// opaca local, ABI-idéntica (puntero) a la del header.
typedef struct AddressSpace AddressSpace;

// Espejo del layout Rust: Vma{ u64,u64,u64,u32 } = 32 bytes × 64 + u32.
#define KernelSpaceVmas 64u
typedef struct {
    uint64_t Start;
    uint64_t End;
    uint64_t Flags;
    uint32_t Kind;
    uint32_t Pad;
} KernelVma;
typedef struct {
    KernelVma Vmas[KernelSpaceVmas];
    uint32_t VmaCount;
} KernelAddrSpace;

_Static_assert(sizeof(KernelVma) == 32, "Vma mirror must be 32 bytes");

// Espacio kernel a cero: sin VMAs, como un AddrSpace::New() recién creado.
static KernelAddrSpace KernelSpaceStorage;

__attribute__((weak)) AddressSpace *CurrentAddressSpace(void)
{
    return (AddressSpace *)&KernelSpaceStorage;
}

__attribute__((weak)) void ProcessFaultCurrent(TrapFrame *Frame, uint64_t Cr2)
{
    (void)Cr2;
    // TrapFrame e InterruptFrame comparten offsets en la cola
    // (vector/error/rip/cs/rflags/rsp/ss); solo se usa Panic(reason, frame).
    Panic("user fault", (const InterruptFrame *)Frame);
}
