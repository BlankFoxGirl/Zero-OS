#include "x86/guest_image.h"
#include "iso9660.h"
#include "console.h"

#ifdef __x86_64__

static inline uint16_t read16(const uint8_t *p) {
    auto *v = reinterpret_cast<const volatile uint8_t *>(p);
    return static_cast<uint16_t>(v[0])
         | (static_cast<uint16_t>(v[1]) << 8);
}

static bool is_gpt(const uint8_t *base) {
    auto *v = reinterpret_cast<const volatile uint8_t *>(base + 512);
    return v[0] == 'E' && v[1] == 'F' && v[2] == 'I' && v[3] == ' '
        && v[4] == 'P' && v[5] == 'A' && v[6] == 'R' && v[7] == 'T';
}

static bool is_mbr(const uint8_t *base) {
    return read16(base + 510) == 0xAA55;
}

GuestImageType classify_guest_image(uint64_t hpa, uint64_t size) {
    if (size < 1024) {
        kprintf("guest_image: image too small (%llu bytes)\n",
                (unsigned long long)size);
        return GuestImageType::Unknown;
    }

    auto *base = reinterpret_cast<const uint8_t *>(hpa);

    if (iso_is_valid(base)) {
        kprintf("guest_image: ISO 9660 image detected\n");
        return GuestImageType::ISO9660;
    }

    if (is_gpt(base)) {
        kprintf("guest_image: GPT disk image detected\n");
        return GuestImageType::DiskImage;
    }

    if (is_mbr(base)) {
        kprintf("guest_image: MBR disk image detected\n");
        return GuestImageType::DiskImage;
    }

    kprintf("guest_image: unrecognized image format\n");
    return GuestImageType::Unknown;
}

#endif
