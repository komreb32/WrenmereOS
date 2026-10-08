/* SPDX-License-Identifier: BSD-3-Clause */
// PS/2 keyboard: scancode queue and key decoding.
#ifndef KERNEL_DRIVERS_KEYBOARD_H
#define KERNEL_DRIVERS_KEYBOARD_H

void KeyboardInit(void);
int KeyboardStart(void);
int KeyboardPop(void);
int KeyboardGetChar(void);

#endif
