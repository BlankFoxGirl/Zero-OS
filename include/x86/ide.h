#pragma once

#include "types.h"

void ide_init(uint64_t disk_hpa, uint64_t disk_size);

bool ide_handle_io(uint16_t port, bool is_in, uint32_t size, uint64_t *val);
