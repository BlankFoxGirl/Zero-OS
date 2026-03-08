#pragma once

#include "types.h"

void arch_early_init();

[[noreturn]] void arch_halt();

void arch_serial_putchar(char c);
void arch_serial_write(const char *str, size_t len);

char arch_serial_getchar();
bool arch_serial_has_data();
