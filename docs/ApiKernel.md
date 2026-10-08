# Kernel API

Reference for the interfaces declared under `include/` and implemented under `kernel/`. Query functions return data; callers decide how to present it. Only console and panic APIs print as part of their purpose. Initialization, interrupt, driver, and memory interfaces may change state or hardware as described below.

## Types And Conventions

- Integer types `int8_t` through `uint64_t`, `intptr_t`, and `uintptr_t` are provided by `include/lib/stdint.h`.
- Functions returning `int` normally use `0` for success and `-1` for failure unless another result is documented.
- `PageAlloc` frame sizes and x86 hardware page-table pages are both 4096 bytes.
- RAM values returned by `MmTotalRamBytes()` and `MmUsableRam()` are in bytes. The caller converts them before passing them to `KInfoDec()`.

## Boot And Console

### `kernel/core/kernel.c`

`void KernelEntry(void)` is the C entry point called by stage2. It disables IRQs while initializing the GDT, IDT, PIC, PIT, keyboard, and memory manager; it then reports status through the `K*` console APIs, enables IRQs, and echoes keyboard characters. This is orchestration, not a query API.

### `include/lib/stdio.h`, `kernel/libk/stdio.c`

These APIs are specifically intended to write to the VGA text console:

```c
void KClear(void);
void KPut(char c);
void KPrint(const char *string);
void KPrintln(const char *string);
void KStatus(const char *Label, int Ok);
void KInfo(const char *Label);
void KPrintDec(uint64_t Value);
void KPrintHex(uint64_t Value);
void KInfoDec(const char *Label, uint64_t Value);
```

`KPut` writes a character and handles newline and backspace. `KPrint` and `KPrintln` write strings; the latter appends a newline. `KStatus` displays a label with an OK/failure marker; `KInfo` displays an informational label. `KPrintDec` and `KPrintHex` format decimal and hexadecimal numbers. `KInfoDec` prints a label and a decimal number. Console output is their intended side effect; MM, timer, and driver functions should not print their results themselves.

### `include/kernel/core/panic.h`, `kernel/core/panic.c`

```c
__attribute__((noreturn)) void Panic(const char *Reason,
                                     const InterruptFrame *Frame);
```

Displays the fatal screen and, when provided, the reason and interrupt-frame fields; disables interrupts and halts the CPU. It never returns. Printing is part of the panic function's purpose.

## GDT And Interrupts

### `include/kernel/gdt/gdt.h`, `kernel/gdt/gdt.c`

```c
int GdtInit(void);
```

Loads the kernel GDT, sets data selectors, and reloads CS. Returns `0` after completing the sequence. The GDTR descriptor is a packed 10-byte structure.

### `include/kernel/idt/idt.h`, `kernel/idt/idt.c`, `kernel/idt/isr.S`

The shared interrupt frame and callback type are:

```c
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t Vector, Error;
    uint64_t rip, cs, rflags, rsp, ss;
} InterruptFrame;

typedef void (*IrqHandler)(InterruptFrame *frame);
```

APIs:

```c
void IdtInit(void);
int IdtStart(void);
int PicInit(void);
void IrqRegister(int irq, IrqHandler handler);
void IrqUnregister(int irq);
uint32_t IrqSpuriousCount(int irq);
void IrqEnable(void);
void IrqDisable(void);
```

`IdtInit` installs IDT entries; `IdtStart` is its status-returning wrapper. `PicInit` remaps the 8259 PIC and initially masks all lines. `IrqRegister` stores a callback and unmasks a valid PIC line; `IrqUnregister` removes it and masks the line. `IrqSpuriousCount` returns the count for IRQ lines 0-15, or zero for an invalid index. `IrqEnable` and `IrqDisable` execute `sti` and `cli`.

`IrqDispatch(InterruptFrame *)` is the assembly entry point: it dispatches exceptions to panic, invokes IRQ callbacks, and sends PIC EOIs. It is not declared in the header even though `isr.S` references it. It currently prints a warning when it first detects a spurious IRQ7/IRQ15; this is an exceptional diagnostic side effect inside the dispatcher.

`isr.S` exports `IsrStub0` through `IsrStub47`. The stubs normalize the vector/error-code stack layout and call `IrqDispatch`; they must not be called as ordinary C functions.

## Timer And Drivers

### `include/kernel/core/timer.h`, `kernel/core/timer.c`

```c
void TimerInit(int hz);
int TimerStart(int hz);
uint32_t TimerGetTicks(void);
uint64_t TimerUptimeMs(void);
void TimerSleepMs(uint32_t ms);
```

`TimerInit` programs PIT channel 0 and registers IRQ0 when `hz > 0`. `TimerStart` validates the rate and returns `0` on initialization or `-1` for an invalid rate. `TimerGetTicks` returns ticks since timer startup. `TimerUptimeMs` returns estimated milliseconds, or zero if the timer is not configured. `TimerSleepMs` halts until at least the requested time has elapsed; it does nothing if the timer is uninitialized, the duration is zero, or IRQs are disabled.

### `include/kernel/drivers/keyboard.h`, `kernel/drivers/keyboard.c`

```c
void KeyboardInit(void);
int KeyboardStart(void);
int KeyboardPop(void);
int KeyboardGetChar(void);
```

`KeyboardInit` registers IRQ1. `KeyboardStart` performs that initialization and returns `0`. `KeyboardPop` returns a queued scancode or `-1` when the queue is empty. `KeyboardGetChar` blocks until it gets a translatable key and returns the character; it uses a basic set-1 map and handles Shift/Caps Lock. Scancodes may be dropped when the queue is full.

### `include/kernel/drivers/ps2.h`, `kernel/drivers/ps2.c`

```c
uint8_t Ps2ReadStatus(void);
uint8_t Ps2ReadScancode(void);
```

`Ps2ReadStatus` reads status port `0x64`. `Ps2ReadScancode` waits for the output-buffer bit and reads a byte from port `0x60`; it currently has no timeout and must be called when the controller is in the expected state.

### `include/kernel/io/io.h`

Inline x86 port-I/O APIs:

```c
uint8_t InByte(uint16_t port);
void OutByte(uint16_t port, uint8_t data);
uint16_t InWord(uint16_t port);
void OutWord(uint16_t port, uint16_t data);
uint32_t InDword(uint16_t port);
void OutDword(uint16_t port, uint32_t data);
void InWordBuffer(uint16_t port, void *Buffer, uint64_t Count);
void OutWordBuffer(uint16_t port, const void *Buffer, uint64_t Count);
void IoWait(void);
```

`InByte` reads a port and `OutByte` writes one byte; `InWord`/`OutWord` and `InDword`/`OutDword` do the same for 16- and 32-bit accesses. `InWordBuffer` and `OutWordBuffer` transfer `Count` 16-bit words with `rep insw`/`rep outsw`, the block move used by ATA PIO data transfers. `IoWait` performs a short delay through port `0x80`. These require kernel privilege.

### `include/kernel/drivers/hdd/ata.h`, `kernel/drivers/hdd/ataPort.c`

ATA PIO channel layout, drive descriptors and port-level helpers:

```c
uint8_t AtaReadStatus(const AtaChannel *Channel);
uint8_t AtaReadAltStatus(const AtaChannel *Channel);
void AtaDelay400ns(const AtaChannel *Channel);
void AtaSelectDrive(const AtaChannel *Channel, uint8_t Slave);
int AtaWaitNotBusy(const AtaChannel *Channel, uint32_t TimeoutMs);
int AtaWaitDrq(const AtaChannel *Channel, uint32_t TimeoutMs);
int AtaSoftReset(const AtaChannel *Channel);
```

The header defines the classic channel bases (primary `0x1F0`/`0x3F6` IRQ 14, secondary `0x170`/`0x376` IRQ 15), the command and control register offsets, status and device-control bits, the PIO command codes, the `AtaChannel` and `AtaDrive` structures and the `AtaDrive` flag bits (`Present`, `Lba48`, `Dma`, `Smart`).

The port helpers poll with timeouts measured by `TimerUptimeMs`. `AtaReadStatus` reads the command-block status; `AtaReadAltStatus` reads the control-block port without acknowledging an interrupt. `AtaDelay400ns` performs the four alt-status reads the ATA spec allows as the required settle delay. `AtaSelectDrive` programs the device/head register for master or slave and applies that delay. `AtaWaitNotBusy` returns when BSY clears, `AtaWaitDrq` returns when DRQ sets and fails on ERR/DF; both return `0` on success and `-1` on timeout or a floating bus (status `0xFF`). `AtaSoftReset` pulses SRST through the device-control port and waits for BSY to clear. All functions accept a null channel and return `-1`. Sector-level IDENTIFY and read/write commands are not implemented yet.

## Memory Management

### `include/kernel/mm/mm.h`, `kernel/mm/mm.c`

```c
int MmInit(void);
void MmProbeFault(void);
uint64_t MmTotalPages(void);
uint64_t MmTotalRamBytes(void);
uint64_t MmUsableRam(void);
```

`MmInit` initializes paging and the allocator, registers usable E820 regions, reserves kernel/hardware ranges, and runs a mapping self-test. It returns `0` on success and `-1` on failure. Total queries include the full usable E820 map, while allocations are currently restricted to the first 1 GiB identity-mapped by stage2.

`MmTotalPages` returns the number of 4 KiB frames in usable E820 regions. `MmTotalRamBytes` returns that total in bytes. `MmUsableRam` returns the allocator's current free-frame count in bytes; it decreases on allocation and increases when a frame is successfully freed. None of these query functions prints.

`MmProbeFault` attempts to unmap the address used by the MM test. It should only be called from a fault handler configured for recovery; currently exception dispatch goes to `Panic` and does not call it.

### `include/kernel/mm/page_alloc.h`, `kernel/mm/page_alloc.rs`

Constant: `PageSize` is 4096 bytes.

```c
int PageAllocInit(uint64_t MaxEnd, uint64_t Arena);
uint32_t PageAddRegion(uint64_t Base, uint64_t Length);
uint64_t PageAlloc(void);
uint64_t PageAllocPages(uint32_t Count);
uint64_t PageAllocPagesAt(uint64_t Hint, uint32_t Count);
void PageFree(uint64_t Address);
void PageFreePages(uint64_t Address, uint32_t Count);
void PageReserveRange(uint64_t Base, uint64_t Length);
uint64_t PageArenaBase(void);
uint32_t PageArenaPages(void);
uint32_t PageAllocFreeCount(void);
```

`PageAllocInit` prepares a bitmap at `Arena` for frames below `MaxEnd`; it returns `0` or `-1`. `PageAddRegion` enables complete frames within a region and returns how many it added. `PageAlloc` and `PageAllocPages` use first-fit and return a physical address, or zero on failure. `PageAllocPagesAt` tries an exact address. `PageFree`/`PageFreePages` return frames to the bitmap; `PageReserveRange` removes them. `PageArenaBase` returns the physical base corresponding to bitmap index zero. `PageArenaPages` returns the number of bitmap indices up to `MaxEnd`, not the amount of usable RAM. `PageAllocFreeCount` returns the number of free frames.

`MmInit` currently caps `MaxEnd` at the first 1 GiB because stage2 identity-maps only that range and the allocator returns physical addresses used as pointers. Higher usable E820 ranges are included in total-RAM queries but are not made allocatable until a direct map is implemented.

The allocator tracks initialization separately from the physical base, so frames can be freed even though the pool begins at physical address zero. Address zero remains invalid for exact allocation because zero is the allocation-failure sentinel.

### `include/kernel/mm/paging.h`, `kernel/mm/paging.rs`

Constants: `PagingPageSize` = 4096; `PagingPresent`, `PagingWritable`, and `PagingUser` are page-table flags.

```c
int PagingInit(void);
int PagingMap(uint64_t Virt, uint64_t Phys, uint64_t Flags);
int PagingUnmap(uint64_t Virt);
int PagingTranslate(uint64_t Virt, uint64_t *PhysOut);
int PagingIsEnabled(void);
```

`PagingInit` reads the active PML4 from CR3. `PagingMap` maps one 4 KiB frame using one hardware page-table entry. `PagingUnmap` clears that entry. `PagingTranslate` writes the physical address corresponding to `Virt` into `PhysOut`. These operations return `0` on success or `-1` on failure. `PagingIsEnabled` returns `1` when CR0.PG is set and `0` otherwise.

Page-table frames come from `PageAlloc`; the current implementation assumes their physical addresses are also virtually accessible. `Virt` and `Phys` must be 4 KiB aligned; callers must also use a canonical virtual address and manage mapping/table lifetimes. When walking an existing 2 MiB huge-page entry, paging can split it into 4 KiB entries.

## Internal Symbols

`static` functions in `kernel/` are not cross-module APIs: `Scroll`; formatting helpers in `panic.c`/`idt.c`; the PIT callback and IRQ-state helper; keyboard callbacks/state; bitmap reads/writes and internal allocator reserve/free helpers; and page-table walking/access helpers. `OnPanic` is Rust's panic handler and does not return. GDT/IDT structures and private constants are also implementation details.

## Output Policy

A query API returns a value and does not choose how to display it. For example:

```c
KInfoDec("usable RAM (MiB)", (MmUsableRam() + 1048575ULL) / 1048576ULL);
```

Output belongs to the console layer (`KPrint*`, `KInfo*`, `KStatus`) or to a function whose purpose is to stop in panic. Do not add debug printing to memory, timer, or driver APIs; callers can print returned values or status codes.