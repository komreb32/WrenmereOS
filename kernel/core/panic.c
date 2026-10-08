/* SPDX-License-Identifier: BSD-3-Clause */
#include <lib/stdint.h>
#include <kernel/core/panic.h>

#define VideoMemory    ((volatile uint16_t *)(uintptr_t)0xB8000)
#define ScreenWidth    80
#define ScreenCells    (80 * 25)
#define PanicAttribute 0x1F // white on blue

static uint16_t Cursor = 0;

static void Put(char C)
{
    if (Cursor >= ScreenCells)
        return;

    if (C == '\n')
        Cursor += ScreenWidth - (Cursor % ScreenWidth);
    else
        VideoMemory[Cursor++] = (PanicAttribute << 8) | (uint8_t)C;
}

static void Print(const char *String)
{
    while (*String != '\0')
        Put(*String++);
}

static void PrintHex(uint64_t Value)
{
    char Buffer[17];
    int Position = 16;

    Buffer[16] = '\0';

    do
    {
        Buffer[--Position] = "0123456789abcdef"[Value & 0xF];
        Value >>= 4;
    } while (Value != 0);

    Print("0x");
    Print(&Buffer[Position]);
}

static void PrintField(const char *Name, uint64_t Value)
{
    Print(Name);
    PrintHex(Value);
    Put('\n');
}

// paints the whole screen blue and freezes, the idt calls us on fatal faults
__attribute__((noreturn)) void Panic(const char *Reason, const InterruptFrame *Frame)
{
    __asm__ volatile ("cli");

    for (uint16_t i = 0; i < ScreenCells; i++)
        VideoMemory[i] = (PanicAttribute << 8) | ' ';

    Cursor = 0;

    Print(":( Wrenmere OS has run into a problem\n\n");
    Print("PANIC: ");
    Print(Reason);
    Put('\n');

    if (Frame != 0)
    {
        Put('\n');
        PrintField("vector ", Frame->Vector);
        PrintField("error  ", Frame->Error);
        PrintField("rip    ", Frame->rip);
        PrintField("cs     ", Frame->cs);
        PrintField("rflags ", Frame->rflags);
        PrintField("rsp    ", Frame->rsp);

        if (Frame->Vector == 14)
        {
            uint64_t Fault;

            __asm__ volatile ("mov %%cr2, %0" : "=r"(Fault));
            PrintField("cr2    ", Fault);
        }
    }

    Print("\nSystem halted. Please restart.\n");

    for (;;)
        __asm__ volatile ("hlt");
}
