#include "vm.h"
#include "iso9660.h"
#include "inflate.h"
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

// ── zimg (EFI zboot) header — compressed arm64 kernel ────────────────

struct ZimgHeader {
    uint32_t mz_magic;         // 0x5A4D = "MZ"
    char     zimg_tag[4];      // "zimg"
    uint32_t payload_offset;
    uint32_t payload_size;
    uint32_t reserved[2];
    char     comp_type[4];     // "gzip", "lz4", etc.
};

// ── Memory layout constants ──────────────────────────────────────────
// These must match the values in vm.cpp.

static constexpr uint64_t GUEST_RAM_HPA  = 0x48000000ULL;
static constexpr uint64_t GUEST_RAM_IPA  = 0x40000000ULL;
static constexpr uint64_t GUEST_RAM_SIZE = 256ULL * 1024 * 1024;

// QEMU -device loader places the ISO/Image here (outside guest RAM)
static constexpr uint64_t STAGING_HPA = 0x58000000ULL;

static constexpr uint64_t DEFAULT_TEXT_OFFSET = 0x80000;

// DTB near top of guest RAM
static constexpr uint64_t DTB_OFFSET   = GUEST_RAM_SIZE - (1ULL * 1024 * 1024);
static constexpr uint32_t DTB_MAX_SIZE = 64 * 1024;

static constexpr uint64_t SPSR_EL1H_DAIF = 0x3C5;

// Forward declaration (dtb_gen.cpp)
uint32_t dtb_generate(void *out_buf, uint32_t buf_size,
                      uint64_t ram_base, uint64_t ram_size,
                      uint64_t initrd_start, uint64_t initrd_end);

// ── Alignment-safe memory reads ──────────────────────────────────────
// With EL2 MMU off, memory is Device-type: unaligned access faults.
// volatile prevents the compiler from merging byte loads.

static inline uint32_t safe_read_le32(const void *p) {
    auto *v = reinterpret_cast<const volatile uint8_t *>(p);
    return static_cast<uint32_t>(v[0])
         | (static_cast<uint32_t>(v[1]) << 8)
         | (static_cast<uint32_t>(v[2]) << 16)
         | (static_cast<uint32_t>(v[3]) << 24);
}

static inline uint64_t safe_read_le64(const void *p) {
    auto *v = reinterpret_cast<const volatile uint8_t *>(p);
    uint64_t lo = static_cast<uint64_t>(v[0])
                | (static_cast<uint64_t>(v[1]) << 8)
                | (static_cast<uint64_t>(v[2]) << 16)
                | (static_cast<uint64_t>(v[3]) << 24);
    uint64_t hi = static_cast<uint64_t>(v[4])
                | (static_cast<uint64_t>(v[5]) << 8)
                | (static_cast<uint64_t>(v[6]) << 16)
                | (static_cast<uint64_t>(v[7]) << 24);
    return lo | (hi << 32);
}

// ── Kernel format detection ──────────────────────────────────────────

static bool check_arm64_magic(const void *ptr, Arm64ImageHeader *out) {
    auto *raw = static_cast<const uint8_t *>(ptr);
    uint32_t magic = safe_read_le32(raw + offsetof(Arm64ImageHeader, magic));
    if (magic != ARM64_IMAGE_MAGIC)
        return false;

    out->code0       = safe_read_le32(raw + 0);
    out->code1       = safe_read_le32(raw + 4);
    out->text_offset = safe_read_le64(raw + 8);
    out->image_size  = safe_read_le64(raw + 16);
    out->flags       = safe_read_le64(raw + 24);
    out->magic       = magic;
    return true;
}

static bool check_zimg(const void *ptr, ZimgHeader *out) {
    auto *raw = static_cast<const uint8_t *>(ptr);
    auto *v   = reinterpret_cast<const volatile uint8_t *>(ptr);

    if (v[0] != 'M' || v[1] != 'Z')
        return false;
    if (v[4] != 'z' || v[5] != 'i' || v[6] != 'm' || v[7] != 'g')
        return false;

    out->mz_magic       = safe_read_le32(raw + 0);
    out->payload_offset  = safe_read_le32(raw + 8);
    out->payload_size    = safe_read_le32(raw + 12);
    out->comp_type[0]    = static_cast<char>(v[24]);
    out->comp_type[1]    = static_cast<char>(v[25]);
    out->comp_type[2]    = static_cast<char>(v[26]);
    out->comp_type[3]    = static_cast<char>(v[27]);
    return true;
}

// ── Guest image discovery ────────────────────────────────────────────

struct GuestImages {
    const void *kernel;
    uint64_t    kernel_size;
    uint64_t    text_offset;
    bool        compressed;     // needs gzip decompression
    uint32_t    payload_offset; // offset within kernel to gzip stream
    uint32_t    payload_size;
    const void *initrd;
    uint64_t    initrd_size;
};

// Classify a kernel file: raw Image, zimg (compressed), or unknown.
static bool classify_kernel(const void *data, uint64_t size,
                            GuestImages *img) {
    Arm64ImageHeader arm_hdr{};
    if (check_arm64_magic(data, &arm_hdr)) {
        img->kernel      = data;
        img->kernel_size = size;
        img->text_offset = arm_hdr.text_offset ? arm_hdr.text_offset
                                               : DEFAULT_TEXT_OFFSET;
        img->compressed  = false;
        return true;
    }

    ZimgHeader zhdr{};
    if (check_zimg(data, &zhdr)) {
        if (zhdr.comp_type[0] != 'g' || zhdr.comp_type[1] != 'z' ||
            zhdr.comp_type[2] != 'i' || zhdr.comp_type[3] != 'p') {
            kprintf("guest_loader: zimg uses unsupported compression '%.4s'\n",
                    zhdr.comp_type);
            return false;
        }
        img->kernel         = data;
        img->kernel_size    = size;
        img->text_offset    = DEFAULT_TEXT_OFFSET;
        img->compressed     = true;
        img->payload_offset = zhdr.payload_offset;
        img->payload_size   = zhdr.payload_size;
        return true;
    }

    return false;
}

static bool detect_iso(GuestImages *img) {
    auto *staging = reinterpret_cast<const void *>(STAGING_HPA);

    if (!iso_is_valid(staging))
        return false;

    kprintf("guest_loader: ISO 9660 image detected\n");

    static const char *kernel_paths[] = {
        "boot/vmlinuz-lts",
        "boot/vmlinuz-edge",
        "boot/vmlinuz",
        "vmlinuz-lts",
        "vmlinuz-edge",
        "vmlinuz",
        nullptr,
    };

    IsoFile kernel_file{};
    const char *found_path = nullptr;
    for (auto **p = kernel_paths; *p; p++) {
        if (iso_find_file(staging, *p, &kernel_file)) {
            found_path = *p;
            break;
        }
    }

    if (!found_path) {
        kprintf("guest_loader: no kernel found in ISO\n");
        return false;
    }

    if (!classify_kernel(kernel_file.data, kernel_file.size, img)) {
        kprintf("guest_loader: %s is not a recognized arm64 kernel\n",
                found_path);
        return false;
    }

    kprintf("  kernel      : %s (%llu KiB%s)\n",
            found_path, (unsigned long long)(kernel_file.size / 1024),
            img->compressed ? ", compressed" : "");

    static const char *initrd_paths[] = {
        "boot/initramfs-lts",
        "boot/initramfs-edge",
        "boot/initramfs",
        "boot/initrd-lts",
        "boot/initrd",
        "initramfs-lts",
        "initramfs-edge",
        "initramfs",
        "initrd-lts",
        "initrd",
        nullptr,
    };

    IsoFile initrd_file{};
    for (auto **p = initrd_paths; *p; p++) {
        if (iso_find_file(staging, *p, &initrd_file)) {
            img->initrd      = initrd_file.data;
            img->initrd_size = initrd_file.size;
            kprintf("  initramfs   : %s (%llu KiB)\n",
                    *p, (unsigned long long)(initrd_file.size / 1024));
            break;
        }
    }

    return true;
}

static bool detect_raw_image(GuestImages *img) {
    auto *staging = reinterpret_cast<const void *>(STAGING_HPA);

    if (!classify_kernel(staging, 0, img))
        return false;

    kprintf("guest_loader: raw arm64 %s detected at staging area\n",
            img->compressed ? "zimg" : "Image");

    img->initrd      = nullptr;
    img->initrd_size = 0;
    return true;
}

// ── Load images into guest RAM ───────────────────────────────────────

bool vm_has_linux_image() {
    auto *staging = reinterpret_cast<const void *>(STAGING_HPA);

    if (iso_is_valid(staging))
        return true;

    GuestImages tmp{};
    return classify_kernel(staging, 0, &tmp);
}

bool vm_load_guest_images(VM *vm, uint64_t *out_initrd_ipa,
                          uint64_t *out_initrd_size) {
    UNUSED(vm);
    GuestImages img{};

    if (!detect_iso(&img) && !detect_raw_image(&img))
        return false;

    uint64_t text_offset = img.text_offset;

    if (img.compressed) {
        auto *src = static_cast<const uint8_t *>(img.kernel);
        const void *gz_data = src + img.payload_offset;
        uint64_t gz_len     = img.payload_size;

        void *dst = reinterpret_cast<void *>(GUEST_RAM_HPA + text_offset);
        uint64_t cap = DTB_OFFSET - text_offset;

        kprintf("  decompressing kernel (%u KiB gzip)...\n",
                (unsigned)(gz_len / 1024));
        uint64_t decompressed = gzip_decompress(gz_data, gz_len, dst, cap);
        if (decompressed == 0) {
            kprintf("guest_loader: gzip decompression failed\n");
            return false;
        }

        kprintf("  decompressed %llu KiB -> HPA 0x%llx (IPA 0x%llx)\n",
                (unsigned long long)(decompressed / 1024),
                (unsigned long long)(GUEST_RAM_HPA + text_offset),
                (unsigned long long)(GUEST_RAM_IPA + text_offset));

        // Verify the decompressed image has the ARM64 magic
        Arm64ImageHeader hdr{};
        if (check_arm64_magic(dst, &hdr)) {
            if (hdr.text_offset && hdr.text_offset != text_offset) {
                kprintf("  note: Image text_offset=0x%llx (using 0x%llx)\n",
                        (unsigned long long)hdr.text_offset,
                        (unsigned long long)text_offset);
            }
        }
    } else {
        uintptr_t kernel_dst = GUEST_RAM_HPA + text_offset;
        memcpy(reinterpret_cast<void *>(kernel_dst),
               img.kernel, static_cast<size_t>(img.kernel_size));
        kprintf("  loaded kernel at HPA 0x%llx (IPA 0x%llx)\n",
                (unsigned long long)kernel_dst,
                (unsigned long long)(GUEST_RAM_IPA + text_offset));
    }

    if (img.initrd && img.initrd_size > 0) {
        // Place initrd after the kernel area, 2 MiB aligned
        uint64_t initrd_offset = GUEST_RAM_SIZE / 2;
        uintptr_t initrd_dst = GUEST_RAM_HPA + initrd_offset;

        if (initrd_offset + img.initrd_size > DTB_OFFSET) {
            kprintf("guest_loader: initrd too large for guest RAM\n");
            return false;
        }

        memcpy(reinterpret_cast<void *>(initrd_dst),
               img.initrd, static_cast<size_t>(img.initrd_size));

        *out_initrd_ipa  = GUEST_RAM_IPA + initrd_offset;
        *out_initrd_size = img.initrd_size;
        kprintf("  loaded initrd at HPA 0x%llx (IPA 0x%llx, %llu KiB)\n",
                (unsigned long long)initrd_dst,
                (unsigned long long)(GUEST_RAM_IPA + initrd_offset),
                (unsigned long long)(img.initrd_size / 1024));
    } else {
        *out_initrd_ipa  = 0;
        *out_initrd_size = 0;
    }

    return true;
}

bool vm_boot_linux(VM *vm, uint64_t initrd_ipa, uint64_t initrd_size) {
    uintptr_t kernel_hpa = GUEST_RAM_HPA + DEFAULT_TEXT_OFFSET;
    Arm64ImageHeader hdr{};
    if (!check_arm64_magic(reinterpret_cast<const void *>(kernel_hpa), &hdr)) {
        kprintf("guest_loader: no arm64 Image in guest RAM at HPA 0x%llx\n",
                (unsigned long long)kernel_hpa);
        return false;
    }

    uint64_t text_offset = hdr.text_offset;
    if (text_offset == 0)
        text_offset = DEFAULT_TEXT_OFFSET;

    uint64_t kernel_ipa = GUEST_RAM_IPA + text_offset;

    uint64_t dtb_ipa  = GUEST_RAM_IPA + DTB_OFFSET;
    uintptr_t dtb_hpa = GUEST_RAM_HPA + DTB_OFFSET;

    uint64_t initrd_start = 0, initrd_end = 0;
    if (initrd_ipa && initrd_size > 0) {
        initrd_start = initrd_ipa;
        initrd_end   = initrd_ipa + initrd_size;
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

    memset(&vm->vcpu, 0, sizeof(VCpuContext));
    vm->vcpu.x[0]      = dtb_ipa;
    vm->vcpu.elr_el2    = kernel_ipa;
    vm->vcpu.spsr_el2   = SPSR_EL1H_DAIF;
    vm->vcpu.cpacr_el1  = (3ULL << 20);

    kprintf("  entry       : IPA 0x%llx\n", (unsigned long long)kernel_ipa);
    kprintf("  x0 (dtb)    : IPA 0x%llx\n", (unsigned long long)dtb_ipa);

    return true;
}

#endif /* __aarch64__ */
