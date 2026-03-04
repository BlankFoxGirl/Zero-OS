#pragma once

#include "types.h"

struct IsoFile {
    const void *data;
    uint64_t    size;
};

bool iso_is_valid(const void *iso_base);

bool iso_find_file(const void *iso_base, const char *path, IsoFile *out);
