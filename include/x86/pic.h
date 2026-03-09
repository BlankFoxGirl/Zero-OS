#pragma once

#include "types.h"

void pic_init();

bool pic_handle_io(uint16_t port, bool is_in, uint32_t size, uint64_t *val);

void pic_raise_irq(uint8_t irq);

bool pic_pending(uint8_t *out_vector);
