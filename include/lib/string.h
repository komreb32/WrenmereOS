/* SPDX-License-Identifier: BSD-3-Clause */
// Kernel memory and string helpers. Freestanding (no libc, no dynamic
// memory): every routine is a plain byte loop over uint64_t lengths, the
// house integer for sizes (see lib/stdint.h). On x86-64 uint64_t matches the
// LP64 size_t ABI, so the compiler can also resolve the implicit
// memcpy/memset/memmove/memcmp calls it emits under -ffreestanding against
// these definitions.
#ifndef LIB_STRING_H
#define LIB_STRING_H

#include <lib/stdint.h>

// The kernel is freestanding (no <stddef.h>), so provide NULL here, guarded
// like exf.h does for its own translation units.
#ifndef NULL
#define NULL ((void*)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Copy Length bytes from Source to Destination; regions must not overlap.
void* memcpy(void* Destination, const void* Source, uint64_t Length);
// Copy Length bytes, safe when the regions overlap.
void* memmove(void* Destination, const void* Source, uint64_t Length);
// Fill Length bytes of Destination with (uint8_t)Value.
void* memset(void* Destination, int Value, uint64_t Length);
// Compare Length bytes; returns 0 when equal, otherwise the difference of
// the first differing bytes as unsigned chars (< 0 or > 0).
int memcmp(const void* Left, const void* Right, uint64_t Length);
// Find (uint8_t)Value in the first Length bytes; NULL when absent.
void* memchr(const void* Source, int Value, uint64_t Length);

// Length of a NUL-terminated string, not counting the NUL.
uint64_t strlen(const char* String);
// Length capped at Max (no NUL past Max is read).
uint64_t strnlen(const char* String, uint64_t Max);
// Compare as unsigned chars; 0 when equal, < 0 or > 0 otherwise.
int strcmp(const char* Left, const char* Right);
// Compare at most Length bytes; NUL ends the comparison early.
int strncmp(const char* Left, const char* Right, uint64_t Length);
// Copy Source with its NUL; Destination must hold strlen(Source) + 1.
char* strcpy(char* Destination, const char* Source);
// Copy at most Length bytes, NUL-padding the rest; when Source holds no NUL
// in its first Length bytes, Destination is left unterminated (standard).
char* strncpy(char* Destination, const char* Source, uint64_t Length);
// Append Source with its NUL; Destination must hold both plus the NUL.
char* strcat(char* Destination, const char* Source);
// Append at most Length bytes of Source, always NUL-terminated.
char* strncat(char* Destination, const char* Source, uint64_t Length);
// First occurrence of (char)Char, including the terminating NUL; NULL when
// absent.
char* strchr(const char* String, int Char);
// Last occurrence of (char)Char, including the terminating NUL.
char* strrchr(const char* String, int Char);
// First occurrence of Needle; the empty needle matches at Haystack, NULL
// when there is no match.
char* strstr(const char* Haystack, const char* Needle);

#ifdef __cplusplus
}
#endif

#endif

