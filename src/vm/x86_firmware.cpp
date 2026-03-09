#include "x86/firmware.h"
#include "boot_info.h"
#include "console.h"
#include "string.h"

#ifdef __x86_64__

static bool name_contains(const char *haystack, const char *needle) {
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            char h = haystack[i + j];
            char n = needle[j];
            if (h >= 'A' && h <= 'Z') h += 32;
            if (n >= 'A' && n <= 'Z') n += 32;
            if (h != n) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

static bool looks_like_ovmf(const BootModule *m) {
    if (name_contains(m->name, "ovmf"))
        return true;
    if (name_contains(m->name, ".fd"))
        return true;
    return false;
}

bool ovmf_find_module(const BootInfo *info, FirmwareInfo *out) {
    if (!info) return false;

    for (uint32_t i = 0; i < info->module_count; i++) {
        const auto &m = info->modules[i];
        if (looks_like_ovmf(&m) && m.size > 0 && m.size <= OVMF_MAX_SIZE) {
            out->hpa  = m.hpa;
            out->size = m.size;
            out->guest_base = 0;
            kprintf("firmware: found OVMF module '%s' (%llu KiB)\n",
                    m.name, (unsigned long long)(m.size / 1024));
            return true;
        }
    }

    // If exactly two modules are loaded, the larger one is the disk image
    // and the smaller one is likely the firmware.
    if (info->module_count == 2) {
        const BootModule *smaller = &info->modules[0];
        const BootModule *larger  = &info->modules[1];
        if (smaller->size > larger->size) {
            const BootModule *tmp = smaller;
            smaller = larger;
            larger  = tmp;
        }
        if (smaller->size > 0 && smaller->size <= OVMF_MAX_SIZE) {
            out->hpa  = smaller->hpa;
            out->size = smaller->size;
            out->guest_base = 0;
            kprintf("firmware: using smaller module '%s' as OVMF (%llu KiB)\n",
                    smaller->name, (unsigned long long)(smaller->size / 1024));
            return true;
        }
    }

    return false;
}

#endif
