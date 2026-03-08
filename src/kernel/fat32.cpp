#include "fat32.h"
#include "string.h"
#include "console.h"

// Alignment-safe reads — same pattern as iso9660.cpp.
// On AArch64 with EL2 MMU off, memory is Device-type and unaligned
// access faults.  volatile prevents the compiler from merging byte loads.

static inline uint16_t read_le16(const uint8_t *p) {
    auto *v = reinterpret_cast<const volatile uint8_t *>(p);
    return static_cast<uint16_t>(v[0])
         | (static_cast<uint16_t>(v[1]) << 8);
}

static inline uint32_t read_le32(const uint8_t *p) {
    auto *v = reinterpret_cast<const volatile uint8_t *>(p);
    return static_cast<uint32_t>(v[0])
         | (static_cast<uint32_t>(v[1]) << 8)
         | (static_cast<uint32_t>(v[2]) << 16)
         | (static_cast<uint32_t>(v[3]) << 24);
}

static inline uint8_t read_byte(const uint8_t *p) {
    return *reinterpret_cast<const volatile uint8_t *>(p);
}

// ── BPB offsets (common to FAT12/16/32) ──────────────────────────────

static constexpr uint32_t BPB_BYTES_PER_SECTOR   = 11;
static constexpr uint32_t BPB_SECTORS_PER_CLUSTER = 13;
static constexpr uint32_t BPB_RESERVED_SECTORS    = 14;
static constexpr uint32_t BPB_NUM_FATS            = 16;
static constexpr uint32_t BPB_TOTAL_SECTORS_16    = 19;
static constexpr uint32_t BPB_TOTAL_SECTORS_32    = 32;
static constexpr uint32_t BPB_SECTORS_PER_FAT_32  = 36;
static constexpr uint32_t BPB_ROOT_CLUSTER        = 44;
static constexpr uint32_t BPB_BOOT_SIG_OFFSET     = 510;

// FAT32 cluster chain sentinel values
static constexpr uint32_t FAT32_EOC_MIN = 0x0FFFFFF8;
static constexpr uint32_t FAT32_MASK    = 0x0FFFFFFF;

// Directory entry layout (32 bytes each)
static constexpr uint32_t DIR_ENTRY_SIZE    = 32;
static constexpr uint32_t DIR_ATTR_OFFSET   = 11;
static constexpr uint32_t DIR_CLUSTER_HI    = 20;
static constexpr uint32_t DIR_CLUSTER_LO    = 26;
static constexpr uint32_t DIR_FILE_SIZE     = 28;

static constexpr uint8_t ATTR_READ_ONLY  = 0x01;
static constexpr uint8_t ATTR_HIDDEN     = 0x02;
static constexpr uint8_t ATTR_SYSTEM     = 0x04;
static constexpr uint8_t ATTR_VOLUME_ID  = 0x08;
static constexpr uint8_t ATTR_DIRECTORY  = 0x10;
static constexpr uint8_t ATTR_LONG_NAME  = 0x0F;

// ── Validation ───────────────────────────────────────────────────────

bool fat32_is_valid(const void *base, uint64_t size) {
    if (size < 512)
        return false;

    auto *b = static_cast<const uint8_t *>(base);

    if (read_byte(b + BPB_BOOT_SIG_OFFSET) != 0x55 ||
        read_byte(b + BPB_BOOT_SIG_OFFSET + 1) != 0xAA)
        return false;

    uint16_t bps = read_le16(b + BPB_BYTES_PER_SECTOR);
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096)
        return false;

    uint16_t total16 = read_le16(b + BPB_TOTAL_SECTORS_16);
    uint32_t spf32   = read_le32(b + BPB_SECTORS_PER_FAT_32);
    if (total16 != 0 || spf32 == 0)
        return false;

    return true;
}

// ── Initialization ───────────────────────────────────────────────────

bool fat32_init(Fat32Fs *fs, const void *base, uint64_t size) {
    if (!fat32_is_valid(base, size))
        return false;

    auto *b = static_cast<const uint8_t *>(base);

    fs->base                = b;
    fs->image_size          = size;
    fs->bytes_per_sector    = read_le16(b + BPB_BYTES_PER_SECTOR);
    fs->sectors_per_cluster = read_byte(b + BPB_SECTORS_PER_CLUSTER);
    fs->reserved_sectors    = read_le16(b + BPB_RESERVED_SECTORS);
    fs->num_fats            = read_byte(b + BPB_NUM_FATS);
    fs->sectors_per_fat     = read_le32(b + BPB_SECTORS_PER_FAT_32);
    fs->root_cluster        = read_le32(b + BPB_ROOT_CLUSTER);

    uint32_t total16 = read_le16(b + BPB_TOTAL_SECTORS_16);
    fs->total_sectors = total16 ? total16 : read_le32(b + BPB_TOTAL_SECTORS_32);

    fs->cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->first_data_sector = fs->reserved_sectors +
                            fs->num_fats * fs->sectors_per_fat;

    return true;
}

// ── Cluster operations ───────────────────────────────────────────────

const uint8_t *fat32_cluster_ptr(Fat32Fs *fs, uint32_t cluster) {
    uint64_t sector = fs->first_data_sector +
                      static_cast<uint64_t>(cluster - 2) * fs->sectors_per_cluster;
    uint64_t offset = sector * fs->bytes_per_sector;
    if (offset + fs->cluster_size > fs->image_size)
        return nullptr;
    return fs->base + offset;
}

static uint32_t fat32_next_cluster(Fat32Fs *fs, uint32_t cluster) {
    uint64_t fat_offset = fs->reserved_sectors *
                          static_cast<uint64_t>(fs->bytes_per_sector) +
                          static_cast<uint64_t>(cluster) * 4;
    if (fat_offset + 4 > fs->image_size)
        return FAT32_EOC_MIN;
    return read_le32(fs->base + fat_offset) & FAT32_MASK;
}

static bool is_eoc(uint32_t cluster) {
    return cluster >= FAT32_EOC_MIN;
}

// ── 8.3 name decoding ────────────────────────────────────────────────
// Converts "FOOBAR  ISO" (11 chars) → "foobar.iso" (null-terminated).

static char to_lower(char c) {
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

static void decode_83_name(const uint8_t *raw, char *out) {
    int pos = 0;

    int name_end = 8;
    while (name_end > 0 && read_byte(raw + name_end - 1) == ' ')
        name_end--;

    for (int i = 0; i < name_end; i++)
        out[pos++] = to_lower(static_cast<char>(read_byte(raw + i)));

    int ext_end = 3;
    while (ext_end > 0 && read_byte(raw + 8 + ext_end - 1) == ' ')
        ext_end--;

    if (ext_end > 0) {
        out[pos++] = '.';
        for (int i = 0; i < ext_end; i++)
            out[pos++] = to_lower(static_cast<char>(read_byte(raw + 8 + i)));
    }

    out[pos] = '\0';
}

// ── Directory reading ────────────────────────────────────────────────

uint32_t fat32_read_dir(Fat32Fs *fs, uint32_t dir_cluster,
                        Fat32File *out, uint32_t max_entries) {
    uint32_t count = 0;
    uint32_t cluster = dir_cluster;

    while (!is_eoc(cluster) && cluster >= 2) {
        const uint8_t *data = fat32_cluster_ptr(fs, cluster);
        if (!data)
            break;

        uint32_t entries_per_cluster = fs->cluster_size / DIR_ENTRY_SIZE;

        for (uint32_t i = 0; i < entries_per_cluster && count < max_entries; i++) {
            const uint8_t *entry = data + i * DIR_ENTRY_SIZE;
            uint8_t first = read_byte(entry);

            if (first == 0x00)
                return count;
            if (first == 0xE5)
                continue;

            uint8_t attr = read_byte(entry + DIR_ATTR_OFFSET);
            if (attr == ATTR_LONG_NAME)
                continue;
            if (attr & ATTR_VOLUME_ID)
                continue;

            UNUSED(ATTR_READ_ONLY);
            UNUSED(ATTR_HIDDEN);
            UNUSED(ATTR_SYSTEM);

            Fat32File *f = &out[count];
            decode_83_name(entry, f->name);

            f->first_cluster = (static_cast<uint32_t>(read_le16(entry + DIR_CLUSTER_HI)) << 16) |
                                static_cast<uint32_t>(read_le16(entry + DIR_CLUSTER_LO));
            f->file_size     = read_le32(entry + DIR_FILE_SIZE);
            f->is_directory  = (attr & ATTR_DIRECTORY) != 0;

            count++;
        }

        cluster = fat32_next_cluster(fs, cluster);
    }

    return count;
}

// ── Contiguity check ─────────────────────────────────────────────────
// Follows the cluster chain and verifies every cluster is sequential.
// virtio_blk needs a contiguous backing region, so fragmented files
// cannot be used directly.

bool fat32_file_is_contiguous(Fat32Fs *fs, uint32_t first_cluster,
                              uint32_t file_size) {
    if (file_size == 0)
        return true;

    uint32_t clusters_needed = (file_size + fs->cluster_size - 1) / fs->cluster_size;
    uint32_t current = first_cluster;

    for (uint32_t i = 1; i < clusters_needed; i++) {
        uint32_t next = fat32_next_cluster(fs, current);
        if (next != current + 1)
            return false;
        current = next;
    }

    return true;
}
