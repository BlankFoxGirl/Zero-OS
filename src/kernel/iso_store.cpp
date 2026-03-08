#include "iso_store.h"
#include "fat32.h"
#include "console.h"
#include "string.h"
#include "arch_interface.h"

// ── .iso extension check ─────────────────────────────────────────────

static bool has_iso_extension(const char *name) {
    size_t len = strlen(name);
    if (len < 5)
        return false;
    const char *ext = name + len - 4;
    return (ext[0] == '.' &&
            (ext[1] == 'i' || ext[1] == 'I') &&
            (ext[2] == 's' || ext[2] == 'S') &&
            (ext[3] == 'o' || ext[3] == 'O'));
}

// ── Serial menu ──────────────────────────────────────────────────────

static uint32_t show_iso_menu(IsoEntry *entries, uint32_t count) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  ZeroOS — Select Guest ISO\n");
    kprintf("========================================\n\n");

    for (uint32_t i = 0; i < count; i++) {
        uint64_t size_mib = entries[i].size / (1024 * 1024);
        kprintf("  [%u]  %s  (%llu MiB)\n",
                i + 1, entries[i].name,
                (unsigned long long)size_mib);
    }

    kprintf("\nSelect [1-%u]: ", count);

    for (;;) {
        char c = arch_serial_getchar();

        if (c >= '1' && c <= '9') {
            uint32_t choice = static_cast<uint32_t>(c - '0');
            if (choice >= 1 && choice <= count) {
                arch_serial_putchar(c);
                kprintf("\n\n");
                return choice - 1;
            }
        }
    }
}

// ── Detection and selection ──────────────────────────────────────────

IsoStoreResult iso_store_detect_and_select(uint64_t store_hpa, uint64_t store_size) {
    IsoStoreResult result{};
    result.found = false;

    auto *base = reinterpret_cast<const void *>(store_hpa);

    if (!fat32_is_valid(base, store_size))
        return result;

    kprintf("iso_store: FAT32 filesystem detected in staging area\n");

    Fat32Fs fs{};
    if (!fat32_init(&fs, base, store_size)) {
        kprintf("iso_store: FAT32 init failed\n");
        return result;
    }

    kprintf("iso_store: cluster size %u bytes, %u total sectors\n",
            fs.cluster_size, fs.total_sectors);

    Fat32File dir_entries[FAT32_MAX_DIR_ENTRIES];
    uint32_t dir_count = fat32_read_dir(&fs, fs.root_cluster,
                                        dir_entries, FAT32_MAX_DIR_ENTRIES);

    IsoEntry isos[ISO_STORE_MAX_ENTRIES];
    uint32_t iso_count = 0;

    for (uint32_t i = 0; i < dir_count && iso_count < ISO_STORE_MAX_ENTRIES; i++) {
        Fat32File *f = &dir_entries[i];
        if (f->is_directory || f->file_size == 0)
            continue;
        if (!has_iso_extension(f->name))
            continue;

        if (!fat32_file_is_contiguous(&fs, f->first_cluster, f->file_size)) {
            kprintf("iso_store: WARNING: %s is fragmented, skipping\n", f->name);
            continue;
        }

        const uint8_t *data = fat32_cluster_ptr(&fs, f->first_cluster);
        if (!data)
            continue;

        IsoEntry *e = &isos[iso_count];
        memcpy(e->name, f->name, 13);
        e->hpa  = reinterpret_cast<uint64_t>(data);
        e->size = f->file_size;
        iso_count++;

        kprintf("iso_store: found %s (%llu MiB)\n",
                f->name, (unsigned long long)(f->file_size / (1024 * 1024)));
    }

    if (iso_count == 0)
        return result;

    result.found = true;

    if (iso_count == 1) {
        kprintf("iso_store: single ISO detected, auto-selecting %s\n",
                isos[0].name);
        result.selected_hpa  = isos[0].hpa;
        result.selected_size = isos[0].size;
        return result;
    }

    uint32_t choice = show_iso_menu(isos, iso_count);
    kprintf("iso_store: selected %s\n", isos[choice].name);
    result.selected_hpa  = isos[choice].hpa;
    result.selected_size = isos[choice].size;
    return result;
}
