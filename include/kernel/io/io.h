/* SPDX-License-Identifier: BSD-3-Clause */
// Port I/O for x86-64.
#ifndef KERNEL_IO_H
#define KERNEL_IO_H

#include <lib/stdint.h>

static inline uint8_t InByte(uint16_t port)
{
    uint8_t result;

    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void OutByte(uint16_t port, uint8_t data)
{
    __asm__ volatile ("outb %0, %1" : : "a"(data), "Nd"(port));
}

static inline uint16_t InWord(uint16_t port)
{
    uint16_t result;

    __asm__ volatile ("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void OutWord(uint16_t port, uint16_t data)
{
    __asm__ volatile ("outw %0, %1" : : "a"(data), "Nd"(port));
}

static inline uint32_t InDword(uint16_t port)
{
    uint32_t result;

    __asm__ volatile ("inl %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void OutDword(uint16_t port, uint32_t data)
{
    __asm__ volatile ("outl %0, %1" : : "a"(data), "Nd"(port));
}

// Block transfers of 16-bit words (ATA data port reads and writes).
static inline void InWordBuffer(uint16_t port, void *Buffer, uint64_t Count)
{
    __asm__ volatile ("rep insw"
                      : "+D"(Buffer), "+c"(Count)
                      : "d"(port)
                      : "memory");
}

static inline void OutWordBuffer(uint16_t port, const void *Buffer, uint64_t Count)
{
    __asm__ volatile ("rep outsw"
                      : "+S"(Buffer), "+c"(Count)
                      : "d"(port)
                      : "memory");
}

static inline void IoWait(void)
{
    OutByte(0x80, 0);
}

#endif
