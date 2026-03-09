#pragma once

#include "types.h"

void arch_early_init();

[[noreturn]] void arch_halt();
[[noreturn]] void arch_reboot();

// Non-blocking poll for the Ctrl+Alt+Delete key combination.
// On x86/x86_64 this reads raw PS/2 scancodes and tracks modifier state.
// Returns true once all three keys are held simultaneously.
bool arch_poll_ctrl_alt_del();

void arch_serial_putchar(char c);
void arch_serial_write(const char *str, size_t len);

#ifdef __x86_64__
extern "C" bool ensure_physical_mapped(uint64_t phys_start, uint64_t size);
#endif

char arch_serial_getchar();
bool arch_serial_has_data();

// Console input: polls all available input sources (serial, keyboard).
// On x86/x86_64 this checks both COM1 and the PS/2 keyboard.
// On ARM/AArch64 this checks the UART.
bool arch_console_has_input();
char arch_console_getchar();
