/* SPDX-License-Identifier: BSD-3-Clause */
// Kernel memory and string helpers, see include/lib/string.h. Plain byte
// loops only: no libc, no dynamic memory, no calls between routines (so the
// compiler never needs a libcall to satisfy one of these definitions).
#include <lib/string.h>

void* memcpy(void* Destination, const void* Source, uint64_t Length)
{
    uint8_t* destination = (uint8_t*)Destination;
    const uint8_t* source = (const uint8_t*)Source;

    for (uint64_t index = 0; index < Length; index++)
        destination[index] = source[index];

    return Destination;
}

void* memmove(void* Destination, const void* Source, uint64_t Length)
{
    uint8_t* destination = (uint8_t*)Destination;
    const uint8_t* source = (const uint8_t*)Source;

    if (destination < source)
    {
        for (uint64_t index = 0; index < Length; index++)
            destination[index] = source[index];
    }
    else if (destination > source)
    {
        for (uint64_t index = Length; index > 0; index--)
            destination[index - 1] = source[index - 1];
    }

    return Destination;
}

void* memset(void* Destination, int Value, uint64_t Length)
{
    uint8_t* destination = (uint8_t*)Destination;
    uint8_t value = (uint8_t)Value;

    for (uint64_t index = 0; index < Length; index++)
        destination[index] = value;

    return Destination;
}

int memcmp(const void* Left, const void* Right, uint64_t Length)
{
    const uint8_t* left = (const uint8_t*)Left;
    const uint8_t* right = (const uint8_t*)Right;

    for (uint64_t index = 0; index < Length; index++)
    {
        if (left[index] != right[index])
            return (int)left[index] - (int)right[index];
    }

    return 0;
}

void* memchr(const void* Source, int Value, uint64_t Length)
{
    const uint8_t* source = (const uint8_t*)Source;
    uint8_t value = (uint8_t)Value;

    for (uint64_t index = 0; index < Length; index++)
    {
        if (source[index] == value)
            return (void*)&source[index];
    }

    return NULL;
}

uint64_t strlen(const char* String)
{
    uint64_t length = 0;

    while (String[length] != '\0')
        length++;

    return length;
}

uint64_t strnlen(const char* String, uint64_t Max)
{
    uint64_t length = 0;

    while (length < Max && String[length] != '\0')
        length++;

    return length;
}

int strcmp(const char* Left, const char* Right)
{
    const uint8_t* left = (const uint8_t*)Left;
    const uint8_t* right = (const uint8_t*)Right;

    while (*left == *right && *left != '\0')
    {
        left++;
        right++;
    }

    return (int)*left - (int)*right;
}

int strncmp(const char* Left, const char* Right, uint64_t Length)
{
    const uint8_t* left = (const uint8_t*)Left;
    const uint8_t* right = (const uint8_t*)Right;

    for (uint64_t index = 0; index < Length; index++)
    {
        if (left[index] != right[index] || left[index] == '\0')
            return (int)left[index] - (int)right[index];
    }

    return 0;
}

char* strcpy(char* Destination, const char* Source)
{
    uint64_t index = 0;

    while (Source[index] != '\0')
    {
        Destination[index] = Source[index];
        index++;
    }
    Destination[index] = '\0';

    return Destination;
}

char* strncpy(char* Destination, const char* Source, uint64_t Length)
{
    uint64_t index = 0;

    while (index < Length && Source[index] != '\0')
    {
        Destination[index] = Source[index];
        index++;
    }
    while (index < Length)
    {
        Destination[index] = '\0';
        index++;
    }

    return Destination;
}

char* strcat(char* Destination, const char* Source)
{
    uint64_t end = 0;

    while (Destination[end] != '\0')
        end++;

    uint64_t index = 0;
    while (Source[index] != '\0')
    {
        Destination[end + index] = Source[index];
        index++;
    }
    Destination[end + index] = '\0';

    return Destination;
}

char* strncat(char* Destination, const char* Source, uint64_t Length)
{
    uint64_t end = 0;

    while (Destination[end] != '\0')
        end++;

    uint64_t index = 0;
    while (index < Length && Source[index] != '\0')
    {
        Destination[end + index] = Source[index];
        index++;
    }
    Destination[end + index] = '\0';

    return Destination;
}

char* strchr(const char* String, int Char)
{
    char value = (char)Char;

    for (;; String++)
    {
        if (*String == value)
            return (char*)String;
        if (*String == '\0')
            return NULL;
    }
}

char* strrchr(const char* String, int Char)
{
    char value = (char)Char;
    const char* found = NULL;

    for (;; String++)
    {
        if (*String == value)
            found = String;
        if (*String == '\0')
            return (char*)found;
    }
}

char* strstr(const char* Haystack, const char* Needle)
{
    if (*Needle == '\0')
        return (char*)Haystack;

    for (; *Haystack != '\0'; Haystack++)
    {
        uint64_t index = 0;
        while (Needle[index] != '\0' && Haystack[index] == Needle[index])
            index++;

        if (Needle[index] == '\0')
            return (char*)Haystack;
    }

    return NULL;
}

