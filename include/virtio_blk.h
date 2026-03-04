#pragma once

#include "types.h"

void virtio_blk_init(uintptr_t backing_hpa, uint64_t backing_size,
                     uintptr_t guest_ram_hpa, uint64_t guest_ram_ipa);

bool virtio_blk_mmio_access(uint64_t ipa, bool is_write, uint32_t width,
                            uint64_t *val);

void virtio_blk_check_irq();

uint32_t virtio_blk_kick_count();
uint32_t virtio_blk_req_count();
