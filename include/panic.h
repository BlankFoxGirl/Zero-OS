#pragma once

#define KERNEL_PANIC(msg) kernel_panic(__FILE__, __LINE__, (msg))

[[noreturn]] void kernel_panic(const char *file, int line, const char *msg);
