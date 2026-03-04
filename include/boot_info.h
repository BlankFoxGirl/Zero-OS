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
};

[[noreturn]] void kernel_start(const BootInfo &info);
