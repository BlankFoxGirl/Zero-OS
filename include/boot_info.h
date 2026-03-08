#pragma once

#include "types.h"

constexpr uint32_t MEMORY_AVAILABLE        = 1;
constexpr uint32_t MEMORY_RESERVED         = 2;
constexpr uint32_t MEMORY_ACPI_RECLAIMABLE = 3;
constexpr uint32_t MEMORY_NVS              = 4;
constexpr uint32_t MEMORY_BADRAM           = 5;

constexpr uint32_t MAX_MEMORY_REGIONS = 64;

struct MemoryRegion {
    uint64_t base;
    uint64_t length;
    uint32_t type;
};

struct BootInfo {
    const char  *arch_name;
    MemoryRegion memory_regions[MAX_MEMORY_REGIONS];
    uint32_t     memory_region_count;
    uint64_t     total_ram;     // total detected RAM (all regions)
    uint64_t     ram_base;      // base address of primary RAM region
};

// ── Memory layout policy ─────────────────────────────────────────────
// Computed from detected RAM:
//   - 128 MiB reserved for ZeroOS kernel
//   - 25% of total RAM for guest OS RAM
//   - Remaining RAM for guest OS virtual hard drive (ramdisk backing)

struct MemoryLayout {
    uint64_t ram_base;
    uint64_t total_ram;
    uint64_t zeroos_size;
    uint64_t guest_ram_hpa;
    uint64_t guest_ram_ipa;     // identity-mapped (= guest_ram_hpa)
    uint64_t guest_ram_size;
    uint64_t ramdisk_hpa;
    uint64_t ramdisk_size;
};

MemoryLayout compute_memory_layout(uint64_t ram_base, uint64_t total_ram);

[[noreturn]] void kernel_start(const BootInfo &info);
