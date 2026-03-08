#include "boot_info.h"
#include "console.h"
#include "memory.h"
#include "arch_interface.h"

extern "C" void vm_run_test_guest(const MemoryLayout *layout);

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

static constexpr uint64_t ZEROOS_RESERVED = 128ULL * 1024 * 1024;
static constexpr uint64_t BLOCK_2M        = 2ULL * 1024 * 1024;

MemoryLayout compute_memory_layout(uint64_t ram_base, uint64_t total_ram) {
    MemoryLayout layout{};
    layout.ram_base    = ram_base;
    layout.total_ram   = total_ram;
    layout.zeroos_size = ZEROOS_RESERVED;

    if (total_ram <= ZEROOS_RESERVED) {
        layout.guest_ram_hpa  = ram_base + ZEROOS_RESERVED;
        layout.guest_ram_ipa  = layout.guest_ram_hpa;
        layout.guest_ram_size = 0;
        layout.ramdisk_hpa    = layout.guest_ram_hpa;
        layout.ramdisk_size   = 0;
        return layout;
    }

    uint64_t guest_ram = ALIGN_DOWN(total_ram / 4, BLOCK_2M);
    uint64_t remaining = total_ram - ZEROOS_RESERVED - guest_ram;

    layout.guest_ram_hpa  = ram_base + ZEROOS_RESERVED;
    layout.guest_ram_ipa  = layout.guest_ram_hpa;
    layout.guest_ram_size = guest_ram;
    layout.ramdisk_hpa    = layout.guest_ram_hpa + guest_ram;
    layout.ramdisk_size   = remaining;

    return layout;
}

[[noreturn]] void kernel_start(const BootInfo &info) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  ZeroOS v0.1.0\n");
    kprintf("  Architecture: %s\n", info.arch_name);
    kprintf("========================================\n\n");

    kprintf("Memory map (%u regions, PMM-managed):\n", info.memory_region_count);
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

    kprintf("Host RAM detected: %llu MiB at 0x%llx\n",
            (unsigned long long)(info.total_ram / (1024 * 1024)),
            (unsigned long long)info.ram_base);

    MemoryLayout layout = compute_memory_layout(info.ram_base, info.total_ram);

    kprintf("\nMemory allocation:\n");
    kprintf("  ZeroOS kernel : %llu MiB  [0x%llx - 0x%llx)\n",
            (unsigned long long)(layout.zeroos_size / (1024 * 1024)),
            (unsigned long long)layout.ram_base,
            (unsigned long long)(layout.ram_base + layout.zeroos_size));
    kprintf("  Guest OS RAM  : %llu MiB  [0x%llx - 0x%llx)  (25%%)\n",
            (unsigned long long)(layout.guest_ram_size / (1024 * 1024)),
            (unsigned long long)layout.guest_ram_hpa,
            (unsigned long long)(layout.guest_ram_hpa + layout.guest_ram_size));
    kprintf("  Guest OS Disk : %llu MiB  [0x%llx - 0x%llx)\n",
            (unsigned long long)(layout.ramdisk_size / (1024 * 1024)),
            (unsigned long long)layout.ramdisk_hpa,
            (unsigned long long)(layout.ramdisk_hpa + layout.ramdisk_size));
    kprintf("\n");

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

    kprintf("\nKernel initialisation complete.\n");

    vm_run_test_guest(&layout);

    kprintf("System halting.\n");
    arch_halt();
}
