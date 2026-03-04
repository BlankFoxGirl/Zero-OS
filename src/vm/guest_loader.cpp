#include "vm.h"
#include "string.h"
#include "console.h"

#ifdef __aarch64__

// ── arm64 Linux Image header (at byte offset 0 of the Image file) ────

struct Arm64ImageHeader {
    uint32_t code0;
    uint32_t code1;
    uint64_t text_offset;
    uint64_t image_size;
    uint64_t flags;
    uint64_t res2;
    uint64_t res3;
    uint64_t res4;
    uint32_t magic;        // 0x644d5241 = "ARM\x64"
    uint32_t pe_offset;
};

static constexpr uint32_t ARM64_IMAGE_MAGIC = 0x644d5241;

// ── Guest memory layout constants ────────────────────────────────────

static constexpr uint64_t GUEST_RAM_HPA  = 0x48000000ULL;
static constexpr uint64_t GUEST_RAM_IPA  = 0x40000000ULL;
static constexpr uint64_t GUEST_RAM_SIZE = 64ULL * 1024 * 1024;

// Default kernel load offset (used if header text_offset is 0)
static constexpr uint64_t DEFAULT_TEXT_OFFSET = 0x80000;

// DTB is placed near the top of guest RAM (within first 512 MiB from kernel,
// as required by Linux arm64 boot protocol)
static constexpr uint64_t DTB_OFFSET  = GUEST_RAM_SIZE - (1ULL * 1024 * 1024);
static constexpr uint32_t DTB_MAX_SIZE = 64 * 1024;

// SPSR for initial guest entry: EL1h, DAIF masked, D-cache and MMU off
static constexpr uint64_t SPSR_EL1H_DAIF = 0x3C5;

// Forward declaration (dtb_gen.cpp)
uint32_t dtb_generate(void *out_buf, uint32_t buf_size,
                      uint64_t ram_base, uint64_t ram_size,
                      uint64_t initrd_start, uint64_t initrd_end);

// ── Check if a Linux Image is present at the expected kernel HPA ─────

static const Arm64ImageHeader *find_kernel_header() {
    uintptr_t kernel_hpa = GUEST_RAM_HPA + DEFAULT_TEXT_OFFSET;
    auto *hdr = reinterpret_cast<const Arm64ImageHeader *>(kernel_hpa);

    if (hdr->magic == ARM64_IMAGE_MAGIC)
        return hdr;

    return nullptr;
}

// ── Boot Linux in the VM ─────────────────────────────────────────────

bool vm_boot_linux(VM *vm, uint64_t initrd_ipa, uint64_t initrd_size) {
    const auto *hdr = find_kernel_header();
    if (!hdr) {
        kprintf("guest_loader: no arm64 Linux Image found at HPA 0x%llx\n",
                (unsigned long long)(GUEST_RAM_HPA + DEFAULT_TEXT_OFFSET));
        return false;
    }

    uint64_t text_offset = hdr->text_offset;
    if (text_offset == 0)
        text_offset = DEFAULT_TEXT_OFFSET;

    uint64_t image_size = hdr->image_size;
    kprintf("guest_loader: arm64 Image found\n");
    kprintf("  text_offset : 0x%llx\n", (unsigned long long)text_offset);
    kprintf("  image_size  : 0x%llx (%llu KiB)\n",
            (unsigned long long)image_size,
            (unsigned long long)(image_size / 1024));

    uint64_t kernel_ipa = GUEST_RAM_IPA + text_offset;

    // Generate DTB in guest RAM
    uint64_t dtb_ipa = GUEST_RAM_IPA + DTB_OFFSET;
    uintptr_t dtb_hpa = GUEST_RAM_HPA + DTB_OFFSET;

    uint64_t initrd_start = 0, initrd_end = 0;
    if (initrd_ipa && initrd_size > 0) {
        initrd_start = initrd_ipa;
        initrd_end   = initrd_ipa + initrd_size;
        kprintf("  initrd      : IPA 0x%llx - 0x%llx (%llu KiB)\n",
                (unsigned long long)initrd_start,
                (unsigned long long)initrd_end,
                (unsigned long long)(initrd_size / 1024));
    }

    uint32_t dtb_size = dtb_generate(
        reinterpret_cast<void *>(dtb_hpa), DTB_MAX_SIZE,
        GUEST_RAM_IPA, GUEST_RAM_SIZE,
        initrd_start, initrd_end);

    if (dtb_size == 0) {
        kprintf("guest_loader: DTB generation failed\n");
        return false;
    }

    kprintf("  dtb         : IPA 0x%llx (%u bytes)\n",
            (unsigned long long)dtb_ipa, dtb_size);

    // Set up vCPU for arm64 Linux boot protocol:
    //   x0 = DTB physical address (in guest IPA space)
    //   x1 = x2 = x3 = 0
    //   PC = kernel entry (text_offset from RAM base)
    //   MMU off, D-cache may be on or off
    //   EL1h with DAIF masked
    memset(&vm->vcpu, 0, sizeof(VCpuContext));
    vm->vcpu.x[0]      = dtb_ipa;
    vm->vcpu.elr_el2    = kernel_ipa;
    vm->vcpu.spsr_el2   = SPSR_EL1H_DAIF;
    vm->vcpu.cpacr_el1  = (3ULL << 20);   // enable FP/SIMD

    kprintf("  entry       : IPA 0x%llx\n", (unsigned long long)kernel_ipa);
    kprintf("  x0 (dtb)    : IPA 0x%llx\n", (unsigned long long)dtb_ipa);

    return true;
}

// ── Check if Linux Image is present (called from kernel_start) ───────

bool vm_has_linux_image() {
    return find_kernel_header() != nullptr;
}

#endif /* __aarch64__ */
