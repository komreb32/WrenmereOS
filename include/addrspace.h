/* SPDX-License-Identifier: BSD-3-Clause */
// C declarations for kernel/mm/addrspace.rs (extern "C", no_mangle).
// Mirrors every `pub unsafe extern "C" fn` in that file. The .rs is the
// source of truth; this header is not included by Rust and changes nothing
// there.
#ifndef ADDRSPACE_H
#define ADDRSPACE_H

#include <lib/stdint.h>

// Freestanding: el kernel compila con -nostdinc, así que no hay <stdbool.h>.
// _Bool es palabra clave de C99 y no necesita cabecera; este typedef le da
// el nombre `bool` pedido para las firmas (ABI idéntico al bool de Rust: 1 byte).
typedef _Bool bool;

#ifdef __cplusplus
extern "C" {
#endif

// Opaque address space. NOTE (firma): el .rs la llama `struct AddrSpace`;
// aquí se usa el nombre pedido `struct AddressSpace`. Por puntero opaco el
// ABI es idéntico (un puntero), pero el nombre del tag difiere del símbolo
// Rust. Si quieres coincidencia exacta, renombra a `struct AddrSpace`.
typedef struct AddressSpace AddressSpace;

// VMA con el mismo layout que `#[repr(C)] pub struct Vma` del .rs:
//   Start: u64, End: u64, Flags: u64, Kind: u32 (+4 de relleno al final).
// Se expone completo (no opaco) porque AsFindVma devuelve un puntero a él
// y el llamante C necesita leer Start/End/Flags/Kind.
typedef struct Vma {
    uint64_t Start;
    uint64_t End;
    uint64_t Flags;
    uint32_t Kind;
} Vma;

// Límites / tamaños (valores copiados del .rs).
#define AsPageSize  4096ULL
#define AsUserLimit 0x0000800000000000ULL
#define AsMmapBase  0x0000100000000000ULL
#define AsMaxVmas   64u

// Flags de permiso (bits de página, pasan directos a PagingMap).
// NOTA (nombres): el .rs los llama AsFlagPresent / AsFlagWritable /
// AsFlagUser / AsFlagExecute. Aquí se usan los nombres pedidos; los valores
// son los mismos del .rs.
#define AsPresent 0x001ULL
#define AsWrite   0x002ULL
#define AsUser    0x004ULL
// NOTA (firma): el .rs NO tiene `AsNoExecute`. Solo tiene AsFlagExecute
// (0x200, bit OS que permite ejecutar; el NX hardware va aparte). No hay
// valor equivalente a "no-ejecutable" que copiar; se deja sin definir a
// propósito para no inventar un ABI. Si lo necesitas, dime qué valor debe
// tener (p. ej. el bit NX hardware o 0 = ausencia de AsFlagExecute).
// #define AsNoExecute ??? (sin equivalente en addrspace.rs)

// Tipos de VMA (u32 en el .rs; mismos nombres y valores).
#define VmaKindAnonymous 0u
#define VmaKindStack     1u
#define VmaKindShared    2u
#define VmaKindModule    3u

// Registra [Start, End). 0 ok, -1 si solapa o invade la mitad alta
// (salvo kind Module canónico).
int32_t AsAddVma(AddressSpace *As, uint64_t Start, uint64_t End,
                 uint64_t Flags, uint32_t Kind);
// VMA que contiene Addr, o NULL si ninguna.
Vma *AsFindVma(AddressSpace *As, uint64_t Addr);
// Saca [Start, End) de la lista (parte en dos si cae en medio). 0 ok, -1 error.
int32_t AsRemoveVma(AddressSpace *As, uint64_t Start, uint64_t End);
// Reserva + mapea con marcos a cero. fixed=true usa hint exacto (falla si
// solapa); si no, primer hueco desde hint (o AsMmapBase si hint==0).
// Devuelve la base o 0 en fallo.
uint64_t AsAllocRange(AddressSpace *As, uint64_t Hint, uint64_t Size,
                      uint64_t Flags, uint32_t Kind, bool Fixed);
// Igual pero sin pedir ni mapear marcos (memoria compartida).
uint64_t AsReserveRange(AddressSpace *As, uint64_t Hint, uint64_t Size,
                        uint64_t Flags, uint32_t Kind, bool Fixed);
// Desmapea [Addr, Addr+Size), libera marcos (salvo VMA Shared, que solo
// desmapea) y actualiza las VMAs. 0 ok, -1 error.
int32_t AsFreeRange(AddressSpace *As, uint64_t Addr, uint64_t Size);
// Fallo de página: si Addr cae en VMA Stack/Anonymous con permisos
// suficientes pero sin mapear, pide un marco a cero, lo mapea y devuelve 0;
// si no, -1.
int32_t AsHandleFault(AddressSpace *As, uint64_t Addr, bool Write, bool User);

#ifdef __cplusplus
}
#endif

#endif
