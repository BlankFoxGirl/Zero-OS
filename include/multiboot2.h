#pragma once

#include "types.h"

constexpr uint32_t MULTIBOOT2_BOOTLOADER_MAGIC = 0x36d76289;

struct Multiboot2FixedPart {
    uint32_t total_size;
    uint32_t reserved;
};

struct Multiboot2Tag {
    uint32_t type;
    uint32_t size;
};

constexpr uint32_t MB2_TAG_END         = 0;
constexpr uint32_t MB2_TAG_CMDLINE     = 1;
constexpr uint32_t MB2_TAG_BOOTLOADER  = 2;
constexpr uint32_t MB2_TAG_MMAP        = 6;
constexpr uint32_t MB2_TAG_FRAMEBUFFER = 8;

struct Multiboot2MmapEntry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

struct Multiboot2MmapTag {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
};
