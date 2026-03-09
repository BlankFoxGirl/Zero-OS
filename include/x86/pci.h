#pragma once

#include "types.h"

void pci_init();

bool pci_handle_io(uint16_t port, bool is_in, uint32_t size, uint64_t *val);
