#pragma once

#include <stdarg.h>

void kputchar(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list args);
