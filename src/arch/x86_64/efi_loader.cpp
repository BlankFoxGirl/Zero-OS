#include "x86/efi.h"
#include "boot_info.h"
#include "console.h"
#include "string.h"
#include "arch_interface.h"

#ifdef __x86_64__

static constexpr uint32_t MAX_EFI_IMAGES   = 16;
static constexpr uint64_t READ_CHUNK_SIZE  = 2 * 1024 * 1024;  // 2 MiB

alignas(4096) static uint8_t read_buf[READ_CHUNK_SIZE];

struct EfiImageEntry {
    EFI_FILE_PROTOCOL *dir_root;
    CHAR16             wname[64];
    char               name[64];
    uint64_t           size;
};

// ── Helpers ─────────────────────────────────────────────────────────

static void ucs2_to_ascii(char *dst, const CHAR16 *src, size_t max) {
    size_t i = 0;
    while (src[i] && i + 1 < max) {
        dst[i] = (src[i] < 128) ? static_cast<char>(src[i]) : '?';
        i++;
    }
    dst[i] = 0;
}

static bool has_image_ext(const char *name) {
    size_t len = strlen(name);
    if (len < 5) return false;
    const char *ext = name + len - 4;
    char e[4];
    for (int i = 0; i < 4; i++) {
        e[i] = ext[i];
        if (e[i] >= 'A' && e[i] <= 'Z') e[i] += 32;
    }
    return (e[0] == '.' && e[1] == 'i' && e[2] == 'm' && e[3] == 'g') ||
           (e[0] == '.' && e[1] == 'i' && e[2] == 's' && e[3] == 'o') ||
           (e[0] == '.' && e[1] == 'r' && e[2] == 'a' && e[3] == 'w');
}

// ── Enumerate image files on a single volume ────────────────────────

static uint32_t scan_volume(EFI_FILE_PROTOCOL *root,
                            EfiImageEntry *entries, uint32_t offset,
                            uint32_t max_entries) {
    uint32_t count = 0;
    alignas(8) uint8_t info_buf[512];

    for (;;) {
        uint64_t buf_size = sizeof(info_buf);
        EFI_STATUS st = root->Read(root, &buf_size, info_buf);
        if (st != EFI_SUCCESS || buf_size == 0)
            break;

        auto *fi = reinterpret_cast<EFI_FILE_INFO *>(info_buf);
        if (fi->Attribute & EFI_FILE_DIRECTORY)
            continue;
        if (fi->FileSize == 0)
            continue;

        char ascii_name[64];
        ucs2_to_ascii(ascii_name, fi->FileName, sizeof(ascii_name));

        if (ascii_name[0] == '.')
            continue;
        if (!has_image_ext(ascii_name))
            continue;
        if (offset + count >= max_entries)
            break;

        auto &e = entries[offset + count];
        e.dir_root = root;
        e.size = fi->FileSize;
        memcpy(e.name, ascii_name, sizeof(e.name));
        // Copy UCS-2 name for later Open call
        size_t wi = 0;
        while (fi->FileName[wi] && wi + 1 < 64) {
            e.wname[wi] = fi->FileName[wi];
            wi++;
        }
        e.wname[wi] = 0;

        count++;
    }

    return count;
}

// ── exFAT via EFI Block I/O (fallback) ──────────────────────────────
//
// When the firmware has no exFAT SimpleFileSystem driver, this path
// uses Block I/O to read the exFAT partition directly.  It parses the
// exFAT boot sector and root directory to enumerate guest images.

alignas(4096) static uint8_t fat_buf[4096];

struct ExFatFs {
    EFI_BLOCK_IO_PROTOCOL *bio;
    uint32_t media_id;
    uint32_t sector_size;
    uint32_t cluster_size;
    uint32_t sectors_per_cluster;
    uint64_t fat_lba;
    uint64_t heap_lba;
    uint32_t root_cluster;
};

struct ExFatFile {
    char     name[64];
    uint32_t first_cluster;
    uint64_t data_length;
    bool     contiguous;
};

static bool exfat_probe(EFI_BLOCK_IO_PROTOCOL *bio, ExFatFs *fs) {
    if (!bio->Media || !bio->Media->MediaPresent ||
        !bio->Media->LogicalPartition)
        return false;

    fs->bio         = bio;
    fs->media_id    = bio->Media->MediaId;
    fs->sector_size = bio->Media->BlockSize;

    if (fs->sector_size < 512 || fs->sector_size > 4096)
        return false;

    EFI_STATUS st = bio->ReadBlocks(bio, fs->media_id,
                                     0, fs->sector_size, read_buf);
    if (st != EFI_SUCCESS)
        return false;

    if (read_buf[3] != 'E' || read_buf[4] != 'X' ||
        read_buf[5] != 'F' || read_buf[6] != 'A' ||
        read_buf[7] != 'T')
        return false;

    uint8_t bps_shift = read_buf[108];
    uint8_t spc_shift = read_buf[109];
    fs->sectors_per_cluster = 1u << spc_shift;
    fs->cluster_size = (1u << bps_shift) << spc_shift;
    fs->fat_lba      = *reinterpret_cast<uint32_t *>(read_buf + 80);
    fs->heap_lba     = *reinterpret_cast<uint32_t *>(read_buf + 88);
    fs->root_cluster = *reinterpret_cast<uint32_t *>(read_buf + 96);

    return true;
}

static uint64_t exfat_cluster_lba(const ExFatFs *fs, uint32_t cluster) {
    return fs->heap_lba +
           static_cast<uint64_t>(cluster - 2) * fs->sectors_per_cluster;
}

static uint32_t exfat_fat_read(ExFatFs *fs, uint32_t cluster) {
    uint64_t byte_off   = static_cast<uint64_t>(cluster) * 4;
    uint64_t sector_lba = fs->fat_lba + byte_off / fs->sector_size;
    uint32_t offset_in  = static_cast<uint32_t>(byte_off % fs->sector_size);

    EFI_STATUS st = fs->bio->ReadBlocks(fs->bio, fs->media_id,
                                         sector_lba, fs->sector_size,
                                         fat_buf);
    if (st != EFI_SUCCESS)
        return 0xFFFFFFFF;

    return *reinterpret_cast<uint32_t *>(fat_buf + offset_in);
}

static uint32_t exfat_scan_dir(ExFatFs *fs, ExFatFile *files,
                               uint32_t max_files) {
    uint64_t lba = exfat_cluster_lba(fs, fs->root_cluster);
    uint32_t read_size = fs->cluster_size;
    if (read_size > READ_CHUNK_SIZE)
        read_size = static_cast<uint32_t>(READ_CHUNK_SIZE);

    EFI_STATUS st = fs->bio->ReadBlocks(fs->bio, fs->media_id,
                                         lba, read_size, read_buf);
    if (st != EFI_SUCCESS)
        return 0;

    uint32_t count = 0;
    uint32_t num_entries = read_size / 32;
    auto *raw = read_buf;

    enum { IDLE, GOT_FILE, GOT_STREAM } state = IDLE;
    ExFatFile cur{};
    uint32_t name_off = 0;

    for (uint32_t i = 0; i < num_entries && count < max_files; i++) {
        uint8_t *e = raw + i * 32;
        uint8_t type = e[0];

        if (type == 0x00)
            break;

        switch (type) {
        case 0x85: {
            if (state == GOT_STREAM &&
                cur.name[0] != '.' &&
                has_image_ext(cur.name) && cur.data_length > 0)
                files[count++] = cur;

            uint16_t attrs = *reinterpret_cast<uint16_t *>(e + 4);
            if (attrs & 0x10) {
                state = IDLE;
            } else {
                state = GOT_FILE;
                cur = {};
                name_off = 0;
            }
            break;
        }
        case 0xC0:
            if (state == GOT_FILE) {
                cur.contiguous    = (e[1] & 0x02) != 0;
                cur.first_cluster = *reinterpret_cast<uint32_t *>(e + 20);
                cur.data_length   = *reinterpret_cast<uint64_t *>(e + 24);
                state = GOT_STREAM;
            }
            break;
        case 0xC1:
            if (state == GOT_STREAM) {
                auto *wc = reinterpret_cast<CHAR16 *>(e + 2);
                for (int j = 0; j < 15 && name_off + 1 < sizeof(cur.name); j++) {
                    if (wc[j] == 0) break;
                    cur.name[name_off++] =
                        (wc[j] < 128) ? static_cast<char>(wc[j]) : '?';
                }
                cur.name[name_off] = '\0';
            }
            break;
        default:
            break;
        }
    }

    if (state == GOT_STREAM &&
        cur.name[0] != '.' &&
        has_image_ext(cur.name) && cur.data_length > 0 &&
        count < max_files)
        files[count++] = cur;

    return count;
}

static bool exfat_load_file(ExFatFs *fs, const ExFatFile *file,
                            uint8_t *dst, uint64_t max_size) {
    if (file->data_length > max_size) {
        kprintf("exfat: image too large (%llu MiB > %llu MiB staging area)\n",
                (unsigned long long)(file->data_length / (1024 * 1024)),
                (unsigned long long)(max_size / (1024 * 1024)));
        return false;
    }

    uint64_t remaining = file->data_length;
    uint64_t offset    = 0;
    uint64_t last_pct  = 0;

    if (file->contiguous) {
        uint64_t lba = exfat_cluster_lba(fs, file->first_cluster);
        while (remaining > 0) {
            uint64_t chunk = remaining;
            if (chunk > READ_CHUNK_SIZE)
                chunk = READ_CHUNK_SIZE;
            uint64_t aligned = ALIGN_UP(chunk, fs->sector_size);

            EFI_STATUS st = fs->bio->ReadBlocks(fs->bio, fs->media_id,
                                                 lba, aligned, read_buf);
            if (st != EFI_SUCCESS) {
                kprintf("\nexfat: read error at LBA %llu (0x%llx)\n",
                        (unsigned long long)lba, (unsigned long long)st);
                return false;
            }

            uint64_t copy = (chunk > remaining) ? remaining : chunk;
            memcpy(dst + offset, read_buf, static_cast<size_t>(copy));
            offset    += copy;
            remaining -= copy;
            lba       += aligned / fs->sector_size;

            uint64_t pct = (offset * 100) / file->data_length;
            if (pct != last_pct) {
                kprintf("\refi: loading... %llu%%  (%llu / %llu MiB)",
                        (unsigned long long)pct,
                        (unsigned long long)(offset / (1024 * 1024)),
                        (unsigned long long)(file->data_length / (1024 * 1024)));
                last_pct = pct;
            }
        }
    } else {
        uint32_t cluster = file->first_cluster;
        while (remaining > 0 && cluster >= 2 && cluster < 0xFFFFFFF7) {
            uint64_t lba = exfat_cluster_lba(fs, cluster);
            uint64_t chunk = fs->cluster_size;
            if (chunk > remaining)
                chunk = ALIGN_UP(remaining, fs->sector_size);

            EFI_STATUS st = fs->bio->ReadBlocks(fs->bio, fs->media_id,
                                                 lba, chunk, read_buf);
            if (st != EFI_SUCCESS) {
                kprintf("\nexfat: read error at cluster %u\n", cluster);
                return false;
            }

            uint64_t copy = (chunk > remaining) ? remaining : chunk;
            memcpy(dst + offset, read_buf, static_cast<size_t>(copy));
            offset    += copy;
            remaining -= copy;

            cluster = exfat_fat_read(fs, cluster);

            uint64_t pct = (offset * 100) / file->data_length;
            if (pct != last_pct) {
                kprintf("\refi: loading... %llu%%  (%llu / %llu MiB)",
                        (unsigned long long)pct,
                        (unsigned long long)(offset / (1024 * 1024)),
                        (unsigned long long)(file->data_length / (1024 * 1024)));
                last_pct = pct;
            }
        }
    }

    kprintf("\n");
    return offset == file->data_length;
}

static ExFatFs   saved_exfat_fs;
static ExFatFile saved_exfat_files[MAX_EFI_IMAGES];

static uint32_t scan_block_io_volumes(void *bs,
                                      EfiImageEntry *images,
                                      uint32_t max_images) {
    auto locate_fn = reinterpret_cast<EFI_LOCATE_HANDLE_BUFFER>(
        efi_bs_fn(bs, EBS_OFFSET_LOCATE_HANDLE_BUFFER));
    auto handle_proto_fn = reinterpret_cast<EFI_HANDLE_PROTOCOL>(
        efi_bs_fn(bs, EBS_OFFSET_HANDLE_PROTOCOL));
    auto free_pool_fn = reinterpret_cast<EFI_FREE_POOL>(
        efi_bs_fn(bs, EBS_OFFSET_FREE_POOL));

    uint64_t num_handles = 0;
    EFI_HANDLE *handles = nullptr;
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;

    EFI_STATUS st = locate_fn(EFI_SEARCH_BY_PROTOCOL,
                               &bio_guid, nullptr,
                               &num_handles, &handles);
    if (st != EFI_SUCCESS || num_handles == 0)
        return 0;

    kprintf("efi: scanning %llu block device(s) for exFAT partitions...\n",
            (unsigned long long)num_handles);

    uint32_t total = 0;

    for (uint64_t h = 0; h < num_handles && total < max_images; h++) {
        EFI_BLOCK_IO_PROTOCOL *bio = nullptr;
        st = handle_proto_fn(handles[h], &bio_guid,
                              reinterpret_cast<void **>(&bio));
        if (st != EFI_SUCCESS || !bio)
            continue;

        ExFatFs fs{};
        if (!exfat_probe(bio, &fs))
            continue;

        kprintf("efi: exFAT partition found — cluster size %u, "
                "root cluster %u\n",
                fs.cluster_size, fs.root_cluster);

        saved_exfat_fs = fs;

        uint32_t found = exfat_scan_dir(&fs,
                                        saved_exfat_files + total,
                                        max_images - total);
        for (uint32_t i = 0; i < found; i++) {
            auto &ef = saved_exfat_files[total + i];
            auto &ei = images[total + i];
            memcpy(ei.name, ef.name, sizeof(ei.name));
            ei.size     = ef.data_length;
            ei.dir_root = nullptr;
            memset(ei.wname, 0, sizeof(ei.wname));
            uint32_t idx = total + i;
            memcpy(ei.wname, &idx, sizeof(idx));
        }
        total += found;
    }

    free_pool_fn(handles);
    return total;
}

// ── Interactive menu ────────────────────────────────────────────────

static uint32_t show_efi_menu(EfiImageEntry *entries, uint32_t count) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  ZeroOS — Select Guest Image (EFI)\n");
    kprintf("========================================\n\n");

    for (uint32_t i = 0; i < count; i++) {
        uint64_t size_mib = entries[i].size / (1024 * 1024);
        kprintf("  [%u]  %s  (%llu MiB)\n",
                i + 1, entries[i].name,
                (unsigned long long)size_mib);
    }

    kprintf("\nSelect [1-%u]: ", count);

    for (;;) {
        char c = arch_console_getchar();
        if (c >= '1' && c <= '9') {
            uint32_t choice = static_cast<uint32_t>(c - '0');
            if (choice >= 1 && choice <= count) {
                kprintf("%c\n\n", c);
                return choice - 1;
            }
        }
    }
}

// ── Main entry: scan, select, load ──────────────────────────────────

bool efi_load_guest_image(void *system_table_ptr, void * /*image_handle*/,
                          uint64_t target_hpa, uint64_t max_size,
                          EfiLoadResult *result) {
    result->loaded = false;

    auto *st = reinterpret_cast<EFI_SYSTEM_TABLE *>(system_table_ptr);
    auto *bs = st->BootServices;

    kprintf("efi: scanning UEFI volumes for guest images...\n");

    // LocateHandleBuffer(ByProtocol, &SimpleFileSystemGuid, ...)
    auto locate_fn = reinterpret_cast<EFI_LOCATE_HANDLE_BUFFER>(
        efi_bs_fn(bs, EBS_OFFSET_LOCATE_HANDLE_BUFFER));
    auto handle_proto_fn = reinterpret_cast<EFI_HANDLE_PROTOCOL>(
        efi_bs_fn(bs, EBS_OFFSET_HANDLE_PROTOCOL));
    auto free_pool_fn = reinterpret_cast<EFI_FREE_POOL>(
        efi_bs_fn(bs, EBS_OFFSET_FREE_POOL));

    uint64_t num_handles = 0;
    EFI_HANDLE *handles = nullptr;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

    EFI_STATUS st_loc = locate_fn(EFI_SEARCH_BY_PROTOCOL,
                                   &fs_guid, nullptr,
                                   &num_handles, &handles);
    if (st_loc != EFI_SUCCESS || num_handles == 0) {
        kprintf("efi: no filesystem volumes found (status=0x%llx)\n",
                (unsigned long long)st_loc);
        return false;
    }

    kprintf("efi: found %llu filesystem volume(s)\n",
            (unsigned long long)num_handles);

    EfiImageEntry images[MAX_EFI_IMAGES];
    uint32_t total_images = 0;

    for (uint64_t i = 0; i < num_handles && total_images < MAX_EFI_IMAGES; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = nullptr;
        EFI_STATUS st_hp = handle_proto_fn(handles[i], &fs_guid,
                                            reinterpret_cast<void **>(&fs));
        if (st_hp != EFI_SUCCESS || !fs)
            continue;

        EFI_FILE_PROTOCOL *root = nullptr;
        EFI_STATUS st_ov = fs->OpenVolume(fs, &root);
        if (st_ov != EFI_SUCCESS || !root)
            continue;

        uint32_t found = scan_volume(root, images, total_images,
                                     MAX_EFI_IMAGES);
        if (found == 0) {
            root->Close(root);
            continue;
        }

        total_images += found;
    }

    free_pool_fn(handles);

    // If SimpleFileSystem found nothing, try reading exFAT partitions
    // directly via Block I/O (firmware may lack an exFAT driver).
    if (total_images == 0) {
        kprintf("efi: no images via SimpleFileSystem, "
                "trying Block I/O exFAT fallback...\n");
        total_images = scan_block_io_volumes(bs, images, MAX_EFI_IMAGES);
    }

    if (total_images == 0) {
        kprintf("efi: no guest image files found on any volume\n");
        return false;
    }

    // Display available images and select
    uint32_t choice = 0;
    if (total_images == 1) {
        kprintf("\n");
        kprintf("========================================\n");
        kprintf("  ZeroOS — Guest Images (EFI)\n");
        kprintf("========================================\n\n");
        kprintf("  [1]  %s  (%llu MiB)\n",
                images[0].name,
                (unsigned long long)(images[0].size / (1024 * 1024)));
        kprintf("\nefi: auto-selecting %s\n", images[0].name);
    } else {
        choice = show_efi_menu(images, total_images);
    }

    auto &sel = images[choice];
    kprintf("efi: loading %s (%llu MiB)...\n",
            sel.name, (unsigned long long)(sel.size / (1024 * 1024)));

    if (sel.size > max_size) {
        kprintf("efi: image too large (%llu MiB > %llu MiB staging area)\n",
                (unsigned long long)(sel.size / (1024 * 1024)),
                (unsigned long long)(max_size / (1024 * 1024)));
        return false;
    }

    auto *dst = reinterpret_cast<uint8_t *>(target_hpa);
    uint64_t offset = 0;

    if (sel.dir_root == nullptr) {
        // exFAT Block I/O path
        uint32_t exfat_idx;
        memcpy(&exfat_idx, sel.wname, sizeof(exfat_idx));
        if (!exfat_load_file(&saved_exfat_fs,
                             &saved_exfat_files[exfat_idx],
                             dst, max_size)) {
            kprintf("efi: exFAT load failed for %s\n", sel.name);
            return false;
        }
        offset = sel.size;
    } else {
        // SimpleFileSystem path
        EFI_FILE_PROTOCOL *file = nullptr;
        EFI_STATUS st_open = sel.dir_root->Open(
            sel.dir_root, &file, sel.wname,
            EFI_FILE_MODE_READ, 0);
        if (st_open != EFI_SUCCESS || !file) {
            kprintf("efi: failed to open %s (status=0x%llx)\n",
                    sel.name, (unsigned long long)st_open);
            return false;
        }

        uint64_t remaining = sel.size;
        uint64_t last_pct = 0;

        while (remaining > 0) {
            uint64_t chunk = remaining;
            if (chunk > READ_CHUNK_SIZE)
                chunk = READ_CHUNK_SIZE;

            uint64_t got = chunk;
            EFI_STATUS st_rd = file->Read(file, &got, read_buf);
            if (st_rd != EFI_SUCCESS) {
                kprintf("\nefi: read error at offset %llu (status=0x%llx)\n",
                        (unsigned long long)offset,
                        (unsigned long long)st_rd);
                file->Close(file);
                return false;
            }
            if (got == 0)
                break;

            memcpy(dst + offset, read_buf, static_cast<size_t>(got));
            offset += got;
            remaining -= got;

            uint64_t pct = (offset * 100) / sel.size;
            if (pct != last_pct) {
                kprintf("\refi: loading... %llu%%  (%llu / %llu MiB)",
                        (unsigned long long)pct,
                        (unsigned long long)(offset / (1024 * 1024)),
                        (unsigned long long)(sel.size / (1024 * 1024)));
                last_pct = pct;
            }
        }

        kprintf("\n");
        file->Close(file);
    }

    if (offset != sel.size) {
        kprintf("efi: short read (%llu of %llu bytes)\n",
                (unsigned long long)offset,
                (unsigned long long)sel.size);
    }

    result->loaded = true;
    result->hpa    = target_hpa;
    result->size   = offset;
    memcpy(result->name, sel.name, sizeof(result->name));

    kprintf("efi: loaded %s (%llu MiB) at HPA 0x%llx\n",
            result->name,
            (unsigned long long)(result->size / (1024 * 1024)),
            (unsigned long long)result->hpa);

    // Close remaining SimpleFileSystem roots
    for (uint32_t i = 0; i < total_images; i++) {
        if (images[i].dir_root && images[i].dir_root != sel.dir_root)
            images[i].dir_root->Close(images[i].dir_root);
    }

    return true;
}

// ── Populate BootInfo memory map from EFI GetMemoryMap ──────────────

alignas(8) static uint8_t mmap_buf[16384];

static uint32_t efi_type_to_memory_type(uint32_t efi_type) {
    switch (efi_type) {
    case EFI_LOADER_CODE:
    case EFI_LOADER_DATA:
    case EFI_BOOT_SERVICES_CODE:
    case EFI_BOOT_SERVICES_DATA:
    case EFI_CONVENTIONAL_MEMORY:
        return MEMORY_AVAILABLE;
    case EFI_ACPI_RECLAIM_MEMORY:
        return MEMORY_ACPI_RECLAIMABLE;
    case EFI_ACPI_NVS_MEMORY:
        return MEMORY_NVS;
    case EFI_UNUSABLE_MEMORY:
        return MEMORY_BADRAM;
    default:
        return MEMORY_RESERVED;
    }
}

bool efi_populate_memory_map(void *system_table_ptr, BootInfo *info) {
    auto *st = reinterpret_cast<EFI_SYSTEM_TABLE *>(system_table_ptr);
    auto *bs = st->BootServices;

    auto get_mmap_fn = reinterpret_cast<EFI_GET_MEMORY_MAP>(
        efi_bs_fn(bs, EBS_OFFSET_GET_MEMORY_MAP));

    uint64_t mmap_size = sizeof(mmap_buf);
    uint64_t map_key   = 0;
    uint64_t desc_size = 0;
    uint32_t desc_ver  = 0;

    EFI_STATUS st_mm = get_mmap_fn(&mmap_size, mmap_buf,
                                    &map_key, &desc_size, &desc_ver);
    if (st_mm != EFI_SUCCESS) {
        kprintf("efi: GetMemoryMap failed (0x%llx)\n",
                (unsigned long long)st_mm);
        return false;
    }

    if (desc_size < sizeof(EFI_MEMORY_DESCRIPTOR))
        desc_size = sizeof(EFI_MEMORY_DESCRIPTOR);

    uint64_t num_entries = mmap_size / desc_size;
    info->memory_region_count = 0;

    kprintf("efi: memory map — %llu entries (desc_size=%llu)\n",
            (unsigned long long)num_entries,
            (unsigned long long)desc_size);

    for (uint64_t i = 0; i < num_entries; i++) {
        auto *desc = reinterpret_cast<EFI_MEMORY_DESCRIPTOR *>(
            mmap_buf + i * desc_size);

        uint32_t our_type = efi_type_to_memory_type(desc->Type);
        uint64_t length   = desc->NumberOfPages * 4096ULL;

        if (info->memory_region_count >= MAX_MEMORY_REGIONS) {
            // Merge into last available region if possible, otherwise skip
            if (our_type == MEMORY_AVAILABLE &&
                info->memory_region_count > 0) {
                auto &last = info->memory_regions[info->memory_region_count - 1];
                if (last.type == MEMORY_AVAILABLE &&
                    last.base + last.length == desc->PhysicalStart) {
                    last.length += length;
                    continue;
                }
            }
            break;
        }

        // Merge with previous region if contiguous and same type
        if (info->memory_region_count > 0) {
            auto &prev = info->memory_regions[info->memory_region_count - 1];
            if (prev.type == our_type &&
                prev.base + prev.length == desc->PhysicalStart) {
                prev.length += length;
                continue;
            }
        }

        auto &r = info->memory_regions[info->memory_region_count++];
        r.base   = desc->PhysicalStart;
        r.length = length;
        r.type   = our_type;
    }

    kprintf("efi: consolidated to %u memory regions\n",
            info->memory_region_count);
    return true;
}

// ── ExitBootServices ────────────────────────────────────────────────

void efi_exit_boot_services(void *system_table_ptr, void *image_handle) {
    auto *st = reinterpret_cast<EFI_SYSTEM_TABLE *>(system_table_ptr);
    auto *bs = st->BootServices;

    auto get_mmap_fn = reinterpret_cast<EFI_GET_MEMORY_MAP>(
        efi_bs_fn(bs, EBS_OFFSET_GET_MEMORY_MAP));
    auto exit_bs_fn = reinterpret_cast<EFI_EXIT_BOOT_SERVICES>(
        efi_bs_fn(bs, EBS_OFFSET_EXIT_BOOT_SERVICES));

    // ExitBootServices requires the current memory map key.
    // GetMemoryMap must be called immediately before ExitBootServices
    // with no intervening Boot Service calls.
    for (int retry = 0; retry < 5; retry++) {
        uint64_t mmap_size  = sizeof(mmap_buf);
        uint64_t map_key    = 0;
        uint64_t desc_size  = 0;
        uint32_t desc_ver   = 0;

        EFI_STATUS st_mm = get_mmap_fn(&mmap_size, mmap_buf,
                                        &map_key, &desc_size, &desc_ver);
        if (st_mm != EFI_SUCCESS) {
            kprintf("efi: GetMemoryMap failed (0x%llx), retry %d\n",
                    (unsigned long long)st_mm, retry);
            continue;
        }

        EFI_STATUS st_exit = exit_bs_fn(
            reinterpret_cast<EFI_HANDLE>(image_handle), map_key);
        if (st_exit == EFI_SUCCESS) {
            kprintf("efi: ExitBootServices succeeded\n");
            return;
        }

        kprintf("efi: ExitBootServices failed (0x%llx), retry %d\n",
                (unsigned long long)st_exit, retry);
    }

    kprintf("efi: WARNING — ExitBootServices failed after retries\n");
}

#endif // __x86_64__
