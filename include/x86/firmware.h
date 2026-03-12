#pragma once

#include "types.h"

static constexpr uint64_t OVMF_GPA_END = 0x100000000ULL;
static constexpr uint64_t OVMF_MAX_SIZE = 4 * 1024 * 1024;

struct FirmwareInfo {
    uint64_t hpa;
    uint64_t size;
    uint64_t guest_base;
};

bool ovmf_find_module(const struct BootInfo *info, FirmwareInfo *out);

void ovmf_configure_vmcb();
