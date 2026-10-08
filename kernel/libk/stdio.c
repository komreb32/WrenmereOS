/* SPDX-License-Identifier: BSD-3-Clause */
#include <lib/stdint.h>
#include <lib/stdio.h>

#define VideoMemory   ((volatile uint16_t *)(uintptr_t)0xB8000)
#define ScreenWidth   80
#define ScreenHeight  25
#define ScreenCells   (ScreenWidth * ScreenHeight)
#define TextAttribute 0x07 // light grey on black
#define BlankCell     ((TextAttribute << 8) | ' ')

static uint16_t Cursor = 0;

static void Scroll(void)
{
    for (uint16_t i = 0; i + ScreenWidth < ScreenCells; i++)
        VideoMemory[i] = VideoMemory[i + ScreenWidth];

    for (uint16_t i = ScreenCells - ScreenWidth; i < ScreenCells; i++)
        VideoMemory[i] = BlankCell;

    Cursor -= ScreenWidth;
}

void KClear(void)
{
    for (uint16_t i = 0; i < ScreenCells; i++)
        VideoMemory[i] = BlankCell;

    Cursor = 0;
}

void KPut(char c)
{
    if (c == '\n')
    {
        Cursor += ScreenWidth - (Cursor % ScreenWidth);
    }
    else if (c == '\b')
    {
        // erase the previous character
        if (Cursor > 0)
        {
            Cursor--;
            VideoMemory[Cursor] = BlankCell;
        }
    }
    else
    {
        VideoMemory[Cursor++] = (TextAttribute << 8) | (uint8_t)c;
    }

    if (Cursor >= ScreenCells)
        Scroll();
}

void KPrint(const char *string)
{
    while (*string != '\0')
        KPut(*string++);
}

void KPrintln(const char *string)
{
    KPrint(string);
    KPrint("\n");
}

void KStatus(const char *Label, int Ok)
{
    KPrint(Ok ? "[OK] " : "[FL] ");
    KPrintln(Label);
}

void KInfo(const char *Label)
{
    KPrint("[IF] ");
    KPrintln(Label);
}

void KPrintDec(uint64_t Value)
{
    char Digits[20];
    int i = 0;

    if (Value == 0)
    {
        KPut('0');
        return;
    }

    while (Value != 0 && i < 20)
    {
        Digits[i++] = (char)('0' + (Value % 10));
        Value /= 10;
    }

    while (i-- > 0)
        KPut(Digits[i]);
}

void KPrintHex(uint64_t Value)
{
    char Digits[16];
    int i = 0;

    if (Value == 0)
    {
        KPut('0');
        return;
    }

    while (Value != 0 && i < 16)
    {
        Digits[i++] = "0123456789ABCDEF"[Value & 0xF];
        Value >>= 4;
    }

    while (i-- > 0)
        KPut(Digits[i]);
}

void KInfoDec(const char *Label, uint64_t Value)
{
    KPrint("[IF] ");
    KPrint(Label);
    KPrint(": ");
    KPrintDec(Value);
    KPrint("\n");
}

