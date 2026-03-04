#pragma once

#include "types.h"
#include "result.h"
#include "boot_info.h"

namespace pmm {

void init(const BootInfo &info);

Result<uintptr_t> alloc_page();
void free_page(uintptr_t addr);

uint64_t free_page_count();
uint64_t total_page_count();

}
