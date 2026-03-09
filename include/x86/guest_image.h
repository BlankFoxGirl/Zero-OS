#pragma once

#include "types.h"

enum class GuestImageType : uint32_t {
    ISO9660,
    RawKernel,
    DiskImage,
    Unknown,
};

GuestImageType classify_guest_image(uint64_t hpa, uint64_t size);
