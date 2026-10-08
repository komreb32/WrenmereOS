/* SPDX-License-Identifier: BSD-3-Clause */
// System panic: blue screen with the fault details, then halt.
#ifndef KERNEL_CORE_PANIC_H
#define KERNEL_CORE_PANIC_H

#include <kernel/idt/idt.h>

__attribute__((noreturn)) void Panic(const char *Reason, const InterruptFrame *Frame);

#endif
