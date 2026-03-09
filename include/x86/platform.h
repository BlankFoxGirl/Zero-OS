#pragma once

#include "types.h"

void platform_stubs_init();

bool pit_handle_io(uint16_t port, bool is_in, uint32_t size, uint64_t *val);

bool cmos_handle_io(uint16_t port, bool is_in, uint32_t size, uint64_t *val);

bool ps2_handle_io(uint16_t port, bool is_in, uint32_t size, uint64_t *val);
