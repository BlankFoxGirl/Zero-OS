#pragma once

#include "types.h"

void arch_early_init();

[[noreturn]] void arch_halt();

void arch_serial_putchar(char c);
void arch_serial_write(const char *str, size_t len);

char arch_serial_getchar();
bool arch_serial_has_data();

// Console input: polls all available input sources (serial, keyboard).
// On x86/x86_64 this checks both COM1 and the PS/2 keyboard.
// On ARM/AArch64 this checks the UART.
bool arch_console_has_input();
char arch_console_getchar();
