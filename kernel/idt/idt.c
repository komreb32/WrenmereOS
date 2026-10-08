/* SPDX-License-Identifier: BSD-3-Clause */
#include <lib/stdint.h>
#include <lib/stdio.h>
#include <kernel/core/panic.h>
#include <kernel/gdt/gdt.h>
#include <kernel/idt/idt.h>
#include <kernel/io/io.h>

// 8259 PIC ports and commands
#define Pic1Command 0x20
#define Pic1Data    0x21
#define Pic2Command 0xA0
#define Pic2Data    0xA1
#define PicEoi      0x20
#define IrqBase     0x20
#define IrqCount    16
#define InterruptVectorCount 256
#define StubCount   (IrqBase + IrqCount) // exceptions + pic lines

// OCW3 selects which register the command port returns on the next read
#define PicOcw3Irr   0x0A
#define PicOcw3Isr   0x0B
#define PicInService 0x80

// 64-bit gate: the handler offset is split in three pieces
typedef struct {
    uint16_t OffsetLow;
    uint16_t Selector;
    uint8_t  Ist;
    uint8_t  TypeAttr;
    uint16_t OffsetMid;
    uint32_t OffsetHigh;
    uint32_t Reserved;
} __attribute__((packed)) IdtEntry;

typedef struct {
    uint16_t Limit;
    uint64_t Base;
} __attribute__((packed)) IdtDescriptor;

static IdtEntry Idt[InterruptVectorCount];
static IdtDescriptor Idtr;
static IrqHandler Handlers[InterruptVectorCount];
static uint32_t SpuriousCount[IrqCount];
static uint32_t SpuriousReported = 0;

static void PrintHex(uint64_t value)
{
    char buffer[17];
    int cursor = 16;

    buffer[16] = '\0';

    do
    {
        buffer[--cursor] = "0123456789abcdef"[value & 0xF];
        value >>= 4;
    } while (value != 0);

    KPrint("0x");
    KPrint(&buffer[cursor]);
}

// fatal cpu faults land on the blue screen instead of the old halt loop
static void CpuPanic(InterruptFrame *frame)
{
    static const char *Names[] = {
        [0]  = "divide error (#DE)",
        [1]  = "debug exception (#DB)",
        [2]  = "non-maskable interrupt (#NMI)",
        [3]  = "breakpoint (#BP)",
        [4]  = "overflow (#OF)",
        [5]  = "bound range exceeded (#BR)",
        [6]  = "invalid opcode (#UD)",
        [7]  = "device not available (#NM)",
        [8]  = "double fault (#DF)",
        [10] = "invalid TSS (#TS)",
        [11] = "segment not present (#NP)",
        [12] = "stack segment fault (#SS)",
        [13] = "general protection fault (#GP)",
        [14] = "page fault (#PF)",
        [16] = "x87 floating point (#MF)",
        [17] = "alignment check (#AC)",
        [18] = "machine check (#MC)",
        [19] = "SIMD floating point (#XF)",
        [20] = "virtualization (#VE)",
        [21] = "control protection (#CP)",
        [30] = "security exception (#SX)",
    };

    const char *Reason = "unknown cpu exception";

    if (frame->Vector < sizeof(Names) / sizeof(Names[0]) && Names[frame->Vector] != 0)
        Reason = Names[frame->Vector];

    Panic(Reason, frame);
}

static void IdtSetGate(int vector, uint64_t address)
{
    Idt[vector].OffsetLow  = address & 0xFFFF;
    Idt[vector].Selector   = KernelCodeSelector;
    Idt[vector].Ist        = 0;
    Idt[vector].TypeAttr   = 0x8E;
    Idt[vector].OffsetMid  = (address >> 16) & 0xFFFF;
    Idt[vector].OffsetHigh = (address >> 32) & 0xFFFFFFFF;
    Idt[vector].Reserved   = 0;
}

// fallback for unused vectors so they cannot crash us
__attribute__((naked)) static void StubPlaceholder(void)
{
    __asm__ ("iretq");
}

// macro table instead of 48 hand written stub declarations
#define ForEachStub(X) \
    X(0)  X(1)  X(2)  X(3)  X(4)  X(5)  X(6)  X(7)   \
    X(8)  X(9)  X(10) X(11) X(12) X(13) X(14) X(15)  \
    X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23)  \
    X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)  \
    X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39)  \
    X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47)

#define DeclareStub(n) extern void IsrStub##n(void);
ForEachStub(DeclareStub)
#undef DeclareStub

static void (*const StubTable[StubCount])(void) = {
#define StubEntry(n) IsrStub##n,
    ForEachStub(StubEntry)
#undef StubEntry
};

void IdtInit(void)
{
    int i;

    // vectors 0-47 get real stubs (exceptions + pic lines)
    for (i = 0; i < StubCount; i++)
        IdtSetGate(i, (uint64_t)(uintptr_t)StubTable[i]);

    // vectors 48-255 get the bare placeholder
    for (; i < InterruptVectorCount; i++)
        IdtSetGate(i, (uint64_t)(uintptr_t)StubPlaceholder);

    Idtr.Limit = (uint16_t)(sizeof(Idt) - 1);
    Idtr.Base  = (uint64_t)(uintptr_t)Idt;

    __asm__ volatile ("lidt %0" : : "m"(Idtr));
}

int IdtStart(void)
{
    IdtInit();
    return 0;
}

// remaps the pic so hardware interrupts don't overwrite cpu exceptions
int PicInit(void)
{
    OutByte(Pic1Command, 0x11);
    OutByte(Pic2Command, 0x11);
    OutByte(Pic1Data, IrqBase);
    OutByte(Pic2Data, IrqBase + 8);
    OutByte(Pic1Data, 0x04);
    OutByte(Pic2Data, 0x02);
    OutByte(Pic1Data, 0x01);
    OutByte(Pic2Data, 0x01);

    // mask every line until a driver registers on it
    OutByte(Pic1Data, 0xFF);
    OutByte(Pic2Data, 0xFF);

    return 0;
}

// registers a handler and opens its hardware line on the pic
void IrqRegister(int irq, IrqHandler handler)
{
    if (irq < 0 || irq >= IrqCount || !handler)
        return;

    Handlers[IrqBase + irq] = handler;

    uint16_t port = (irq < 8) ? Pic1Data : Pic2Data;
    OutByte(port, InByte(port) & ~(uint8_t)(1 << (irq & 7)));

    // the slave also needs the cascade line open
    if (irq >= 8)
        OutByte(Pic1Data, InByte(Pic1Data) & ~(uint8_t)(1 << 2));
}

// mutes the hardware line and drops the handler
void IrqUnregister(int irq)
{
    if (irq < 0 || irq >= IrqCount)
        return;

    Handlers[IrqBase + irq] = 0;

    uint16_t port = (irq < 8) ? Pic1Data : Pic2Data;
    OutByte(port, InByte(port) | (uint8_t)(1 << (irq & 7)));
}

uint32_t IrqSpuriousCount(int irq)
{
    if (irq < 0 || irq >= IrqCount)
        return 0;

    return SpuriousCount[irq];
}

// cpu exceptions, hardware signals and the pic eoi all land here
void IrqDispatch(InterruptFrame *frame)
{
    uint64_t vec = frame->Vector;

    if (vec < IrqBase)
        CpuPanic(frame);

    int line = (int)(vec - IrqBase);

    /*
     * Spurious IRQ7/15: a line can drop before the pic latches it, raising a
     * ghost interrupt. The in-service register tells real from spurious; a
     * ghost on the slave still needs an eoi to the master only.
     */
    if (line == 7 || line == 15)
    {
        uint16_t port = (line == 7) ? Pic1Command : Pic2Command;

        OutByte(port, PicOcw3Isr);
        uint8_t inService = InByte(port);

        OutByte(port, PicOcw3Irr);

        if (!(inService & PicInService))
        {
            SpuriousCount[line]++;

            if (line == 15)
                OutByte(Pic1Command, PicEoi);

            if ((SpuriousReported & (1u << line)) == 0)
            {
                SpuriousReported |= 1u << line;

                KPrint("WARNING: spurious IRQ line ");
                PrintHex((uint64_t)line);
                KPrint(" (ISR ");
                PrintHex(inService);
                KPrint("), check PIC/EOI\n");
            }

            return;
        }
    }

    if (Handlers[vec])
        Handlers[vec](frame);

    // tell the pic we are done
    if (line >= 8)
        OutByte(Pic2Command, PicEoi);
    OutByte(Pic1Command, PicEoi);

    // a handler-requested preemption goes here once sched/ exists
}

void IrqDisable(void)
{
    __asm__ volatile ("cli");
}

void IrqEnable(void)
{
    __asm__ volatile ("sti");
}
