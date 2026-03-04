#include "memory.h"
#include "string.h"
#include "console.h"

extern "C" uint8_t __bss_end[];

namespace {

constexpr uint64_t MAX_PHYSICAL_MEMORY = 4ULL * 1024 * 1024 * 1024;
constexpr uint64_t MAX_PAGES           = MAX_PHYSICAL_MEMORY / PAGE_SIZE;
constexpr uint64_t BITMAP_SIZE         = MAX_PAGES / 8; // 128 KiB

// 1 bit per page: 1 = used, 0 = free
uint8_t  bitmap[BITMAP_SIZE];
uint64_t total_pages;
uint64_t used_pages;

void set_page_used(uint64_t idx) { bitmap[idx / 8] |=  (1 << (idx % 8)); }
void set_page_free(uint64_t idx) { bitmap[idx / 8] &= ~(1 << (idx % 8)); }
bool is_page_used(uint64_t idx)  { return bitmap[idx / 8] & (1 << (idx % 8)); }

} // anonymous namespace

namespace pmm {

void init(const BootInfo &info) {
    memset(bitmap, 0xFF, sizeof(bitmap));
    total_pages = 0;
    used_pages  = 0;

    uintptr_t kernel_end =
        ALIGN_UP(reinterpret_cast<uintptr_t>(__bss_end), PAGE_SIZE);

    for (uint32_t i = 0; i < info.memory_region_count; i++) {
        const auto &r = info.memory_regions[i];
        if (r.type != MEMORY_AVAILABLE)
            continue;

        uint64_t base = ALIGN_UP(r.base, PAGE_SIZE);
        uint64_t end  = ALIGN_DOWN(r.base + r.length, PAGE_SIZE);

        if (base < kernel_end)
            base = kernel_end;
        if (end <= base)
            continue;
        if (end > MAX_PHYSICAL_MEMORY)
            end = MAX_PHYSICAL_MEMORY;

        uint64_t first = base / PAGE_SIZE;
        uint64_t last  = end  / PAGE_SIZE;

        for (uint64_t p = first; p < last; p++)
            set_page_free(p);

        total_pages += (last - first);
    }
}

Result<uintptr_t> alloc_page() {
    for (uint64_t i = 0; i < BITMAP_SIZE; i++) {
        if (bitmap[i] == 0xFF)
            continue;
        for (int bit = 0; bit < 8; bit++) {
            if (!(bitmap[i] & (1 << bit))) {
                uint64_t page = i * 8 + bit;
                set_page_used(page);
                used_pages++;
                return Result<uintptr_t>::ok(
                    static_cast<uintptr_t>(page * PAGE_SIZE));
            }
        }
    }
    return Result<uintptr_t>::err(Error::OutOfMemory);
}

void free_page(uintptr_t addr) {
    uint64_t page = addr / PAGE_SIZE;
    if (page >= MAX_PAGES)
        return;
    if (!is_page_used(page))
        return;
    set_page_free(page);
    if (used_pages > 0)
        used_pages--;
}

uint64_t free_page_count() { return total_pages - used_pages; }
uint64_t total_page_count() { return total_pages; }

} // namespace pmm
