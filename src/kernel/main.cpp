#include "boot_info.h"
#include "console.h"
#include "memory.h"
#include "arch_interface.h"

static const char *memory_type_str(uint32_t type) {
    switch (type) {
    case MEMORY_AVAILABLE:        return "Available";
    case MEMORY_RESERVED:         return "Reserved";
    case MEMORY_ACPI_RECLAIMABLE: return "ACPI Reclaimable";
    case MEMORY_NVS:              return "NVS";
    case MEMORY_BADRAM:           return "Bad RAM";
    default:                      return "Unknown";
    }
}

[[noreturn]] void kernel_start(const BootInfo &info) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  ZeroOS v0.1.0\n");
    kprintf("  Architecture: %s\n", info.arch_name);
    kprintf("========================================\n\n");

    kprintf("Memory map (%u regions):\n", info.memory_region_count);
    uint64_t total_available = 0;
    for (uint32_t i = 0; i < info.memory_region_count; i++) {
        const auto &r = info.memory_regions[i];
        uint64_t end = r.base + r.length;
        kprintf("  %016llx - %016llx  %-16s  %llu KiB\n",
                (unsigned long long)r.base,
                (unsigned long long)(end ? end - 1 : 0),
                memory_type_str(r.type),
                (unsigned long long)(r.length / 1024));
        if (r.type == MEMORY_AVAILABLE)
            total_available += r.length;
    }
    kprintf("  Total available: %llu KiB (%llu MiB)\n\n",
            (unsigned long long)(total_available / 1024),
            (unsigned long long)(total_available / (1024 * 1024)));

    pmm::init(info);
    kprintf("Physical memory manager initialised.\n");
    kprintf("  Usable pages : %llu (%llu KiB)\n",
            (unsigned long long)pmm::total_page_count(),
            (unsigned long long)(pmm::total_page_count() * (PAGE_SIZE / 1024)));
    kprintf("  Free pages   : %llu\n\n",
            (unsigned long long)pmm::free_page_count());

    kprintf("Allocation self-test...\n");
    auto r = pmm::alloc_page();
    if (r.is_ok()) {
        kprintf("  alloc  -> 0x%llx\n", (unsigned long long)r.value());
        pmm::free_page(r.value());
        kprintf("  free   -> ok  (free count: %llu)\n",
                (unsigned long long)pmm::free_page_count());
    } else {
        kprintf("  alloc  -> FAILED (out of memory)\n");
    }

    kprintf("\nKernel initialisation complete. Halting.\n");
    arch_halt();
}
