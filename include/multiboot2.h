#pragma once

#include "types.h"

// ── Multiboot 1 ──────────────────────────────────────────────────────

constexpr uint32_t MULTIBOOT1_BOOTLOADER_MAGIC = 0x2BADB002;

struct Multiboot1Info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
};

constexpr uint32_t MB1_FLAG_MMAP = (1 << 6);

struct Multiboot1MmapEntry {
    uint32_t size;
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
} __attribute__((packed));

// ── Multiboot 2 ──────────────────────────────────────────────────────

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
