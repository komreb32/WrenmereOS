/* SPDX-License-Identifier: BSD-3-Clause */
// Kernel console output.
#ifndef LIB_STDIO_H
#define LIB_STDIO_H

#include <lib/stdint.h>

void KClear(void);
void KPut(char c);
void KPrint(const char *string);
void KPrintln(const char *string);
void KStatus(const char *Label, int Ok);
void KInfo(const char *Label);
void KPrintDec(uint64_t Value);
void KPrintHex(uint64_t Value);
void KInfoDec(const char *Label, uint64_t Value);

#endif
