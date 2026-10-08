/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef KERNEL_GDT_H
#define KERNEL_GDT_H

#define KernelCodeSelector 0x08
#define KernelDataSelector 0x10

int GdtInit(void);

#endif
