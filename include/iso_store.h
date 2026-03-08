#pragma once

#include "types.h"

static constexpr uint32_t ISO_STORE_MAX_ENTRIES = 16;

struct IsoEntry {
    char     name[13];
    uint64_t hpa;
    uint64_t size;
};

struct IsoStoreResult {
    bool     found;
    uint64_t selected_hpa;
    uint64_t selected_size;
};

IsoStoreResult iso_store_detect_and_select(uint64_t store_hpa, uint64_t store_size);
