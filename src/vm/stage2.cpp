#include "vm.h"
#include "memory.h"
#include "string.h"
#include "console.h"

#ifdef __aarch64__

// ── Stage-2 descriptor bits ──────────────────────────────────────────

static constexpr uint64_t S2_VALID      = (1ULL << 0);
static constexpr uint64_t S2_TABLE      = (1ULL << 1);   // + VALID = table
static constexpr uint64_t S2_BLOCK      = (0ULL << 1);   // + VALID = block

static constexpr uint64_t S2_AF         = (1ULL << 10);
static constexpr uint64_t S2_SH_IS      = (3ULL << 8);   // inner-shareable
static constexpr uint64_t S2_S2AP_RW    = (3ULL << 6);   // read/write
static constexpr uint64_t S2_MEMATTR_NORMAL = (0xFULL << 2); // WB cacheable
static constexpr uint64_t S2_MEMATTR_DEVICE = (0x0ULL << 2); // nGnRnE device
static constexpr uint64_t S2_XN         = (1ULL << 54);  // execute-never

static constexpr uint64_t S2_PAGE       = (1ULL << 1);   // + VALID = L3 page

static constexpr uint64_t S2_NORMAL_BLOCK =
    S2_VALID | S2_BLOCK | S2_AF | S2_SH_IS | S2_S2AP_RW | S2_MEMATTR_NORMAL;
static constexpr uint64_t S2_DEVICE_BLOCK =
    S2_VALID | S2_BLOCK | S2_AF | S2_S2AP_RW | S2_MEMATTR_DEVICE | S2_XN;
static constexpr uint64_t S2_NORMAL_PAGE =
    S2_VALID | S2_PAGE | S2_AF | S2_SH_IS | S2_S2AP_RW | S2_MEMATTR_NORMAL;
static constexpr uint64_t S2_DEVICE_PAGE =
    S2_VALID | S2_PAGE | S2_AF | S2_S2AP_RW | S2_MEMATTR_DEVICE | S2_XN;

static constexpr uint64_t S2_TABLE_DESC = S2_VALID | S2_TABLE;

static constexpr uint64_t PAGE_4K   = 0x1000ULL;
static constexpr uint64_t BLOCK_2M  = 0x200000ULL;
static constexpr uint64_t ENTRIES_PER_TABLE = 512;

// ── VTCR_EL2 configuration ──────────────────────────────────────────
//  T0SZ  = 32  → 32-bit IPA (4 GiB)
//  SL0   = 1   → start at level 1
//  IRGN0 = 1   → inner WB/WA
//  ORGN0 = 1   → outer WB/WA
//  SH0   = 3   → inner-shareable
//  TG0   = 0   → 4 KiB granule
//  PS    = 2   → 40-bit PA (1 TiB)
//  Bit 31      → RES1

static constexpr uint64_t VTCR_EL2_VAL =
    (1ULL  << 31) |    // RES1
    (2ULL  << 16) |    // PS = 40-bit
    (0ULL  << 14) |    // TG0 = 4KB
    (3ULL  << 12) |    // SH0 = inner-shareable
    (1ULL  << 10) |    // ORGN0 = WB/WA
    (1ULL  <<  8) |    // IRGN0 = WB/WA
    (1ULL  <<  6) |    // SL0 = 1 (start at level 1)
    (32ULL <<  0);     // T0SZ = 32

// ── helpers ──────────────────────────────────────────────────────────

static uintptr_t alloc_table_page() {
    auto r = pmm::alloc_page();
    if (r.is_err()) return 0;
    uintptr_t pa = r.value();
    memset(reinterpret_cast<void *>(pa), 0, PAGE_SIZE);
    return pa;
}

// Level-1 index for a 32-bit IPA with 4KB granule (bits [31:30])
static uint64_t l1_index(uint64_t ipa) {
    return (ipa >> 30) & 0x3;
}

// Level-2 index (bits [29:21])
static uint64_t l2_index(uint64_t ipa) {
    return (ipa >> 21) & 0x1FF;
}

// Level-3 index (bits [20:12])
static uint64_t l3_index(uint64_t ipa) {
    return (ipa >> 12) & 0x1FF;
}

// ── public API ───────────────────────────────────────────────────────

bool stage2_init(VM *vm) {
    uintptr_t l1 = alloc_table_page();
    if (!l1) {
        kprintf("stage2: failed to allocate L1 table\n");
        return false;
    }
    vm->stage2_root = l1;
    return true;
}

bool stage2_map_range(VM *vm, uint64_t ipa, uint64_t hpa,
                      uint64_t size, bool device) {
    if ((ipa & (BLOCK_2M - 1)) || (hpa & (BLOCK_2M - 1)) ||
        (size & (BLOCK_2M - 1))) {
        kprintf("stage2: addresses/size must be 2 MiB aligned\n");
        return false;
    }

    auto *l1 = reinterpret_cast<uint64_t *>(vm->stage2_root);

    for (uint64_t off = 0; off < size; off += BLOCK_2M) {
        uint64_t cur_ipa = ipa + off;
        uint64_t cur_hpa = hpa + off;

        uint64_t i1 = l1_index(cur_ipa);

        // Ensure L2 table exists for this L1 entry
        if (!(l1[i1] & S2_VALID)) {
            uintptr_t l2 = alloc_table_page();
            if (!l2) {
                kprintf("stage2: failed to allocate L2 table\n");
                return false;
            }
            l1[i1] = (static_cast<uint64_t>(l2) & 0x0000FFFFFFFFF000ULL)
                      | S2_TABLE_DESC;
        }

        auto *l2 = reinterpret_cast<uint64_t *>(l1[i1] & 0x0000FFFFFFFFF000ULL);
        uint64_t i2 = l2_index(cur_ipa);

        uint64_t attrs = device ? S2_DEVICE_BLOCK : S2_NORMAL_BLOCK;
        l2[i2] = (cur_hpa & 0x0000FFFFFFE00000ULL) | attrs;
    }

    return true;
}

bool stage2_map_range_4k(VM *vm, uint64_t ipa, uint64_t hpa,
                         uint64_t size, bool device) {
    if ((ipa & (PAGE_4K - 1)) || (hpa & (PAGE_4K - 1)) ||
        (size & (PAGE_4K - 1))) {
        kprintf("stage2: addresses/size must be 4 KiB aligned\n");
        return false;
    }

    auto *l1 = reinterpret_cast<uint64_t *>(vm->stage2_root);

    for (uint64_t off = 0; off < size; off += PAGE_4K) {
        uint64_t cur_ipa = ipa + off;
        uint64_t cur_hpa = hpa + off;

        uint64_t i1 = l1_index(cur_ipa);

        if (!(l1[i1] & S2_VALID)) {
            uintptr_t l2 = alloc_table_page();
            if (!l2) return false;
            l1[i1] = (static_cast<uint64_t>(l2) & 0x0000FFFFFFFFF000ULL)
                      | S2_TABLE_DESC;
        }

        auto *l2 = reinterpret_cast<uint64_t *>(l1[i1] & 0x0000FFFFFFFFF000ULL);
        uint64_t i2 = l2_index(cur_ipa);

        if (l2[i2] & S2_VALID) {
            if (!(l2[i2] & S2_TABLE)) {
                kprintf("stage2: L2 entry already a 2M block, cannot split\n");
                return false;
            }
        } else {
            uintptr_t l3 = alloc_table_page();
            if (!l3) return false;
            l2[i2] = (static_cast<uint64_t>(l3) & 0x0000FFFFFFFFF000ULL)
                      | S2_TABLE_DESC;
        }

        auto *l3 = reinterpret_cast<uint64_t *>(l2[i2] & 0x0000FFFFFFFFF000ULL);
        uint64_t i3 = l3_index(cur_ipa);

        uint64_t attrs = device ? S2_DEVICE_PAGE : S2_NORMAL_PAGE;
        l3[i3] = (cur_hpa & 0x0000FFFFFFFFF000ULL) | attrs;
    }

    return true;
}

// ── Write VTCR_EL2 and VTTBR_EL2 from C++ ───────────────────────────

extern "C" void stage2_activate(uintptr_t root, uint16_t vmid) {
    uint64_t vtcr = VTCR_EL2_VAL;
    uint64_t vttbr = (static_cast<uint64_t>(vmid) << 48) |
                     (static_cast<uint64_t>(root) & 0x0000FFFFFFFFFFFFULL);

    asm volatile("msr vtcr_el2,  %0" :: "r"(vtcr));
    asm volatile("msr vttbr_el2, %0" :: "r"(vttbr));
    asm volatile("isb");
}

extern "C" void stage2_deactivate() {
    asm volatile("msr vttbr_el2, xzr");
    asm volatile("isb");
}

#endif /* __aarch64__ */
