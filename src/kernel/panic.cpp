#include "panic.h"
#include "console.h"
#include "arch_interface.h"

[[noreturn]] void kernel_panic(const char *file, int line, const char *msg) {
    kprintf("\n\n*** KERNEL PANIC ***\n");
    kprintf("Location: %s:%d\n", file, line);
    kprintf("  %s\n", msg);
    kprintf("\nSystem halted.\n");
    arch_halt();
}
