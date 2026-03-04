#include "iso9660.h"
#include "string.h"
#include "console.h"

// ISO 9660 constants
static constexpr uint32_t SECTOR_SIZE = 2048;
static constexpr uint32_t PVD_SECTOR  = 16;
static constexpr uint32_t PVD_OFFSET  = PVD_SECTOR * SECTOR_SIZE;   // 0x8000

// Volume descriptor types
static constexpr uint8_t VD_TYPE_PRIMARY    = 1;
static constexpr uint8_t VD_TYPE_TERMINATOR = 255;

// Directory record file flags
static constexpr uint8_t DIR_FLAG_DIRECTORY = 0x02;

// ── Helpers ──────────────────────────────────────────────────────────

// volatile prevents the compiler from merging byte loads into a wider
// unaligned access (faults on Device memory when MMU is off at EL2).
static inline uint32_t read_le32(const uint8_t *p) {
    auto *v = reinterpret_cast<const volatile uint8_t *>(p);
    return static_cast<uint32_t>(v[0])
         | (static_cast<uint32_t>(v[1]) << 8)
         | (static_cast<uint32_t>(v[2]) << 16)
         | (static_cast<uint32_t>(v[3]) << 24);
}

static inline uint16_t read_le16(const uint8_t *p) {
    auto *v = reinterpret_cast<const volatile uint8_t *>(p);
    return static_cast<uint16_t>(v[0])
         | (static_cast<uint16_t>(v[1]) << 8);
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z')
        return c - ('a' - 'A');
    return c;
}

// Case-insensitive compare, treating '-' and '_' as equivalent
// (ISO 9660 Level 1 maps '-' to '_' on some implementations).
static bool name_eq(const char *iso_name, uint8_t iso_len,
                    const char *query, uint8_t query_len) {
    if (iso_len != query_len)
        return false;
    for (uint8_t i = 0; i < iso_len; i++) {
        char a = to_upper(iso_name[i]);
        char b = to_upper(query[i]);
        if (a == '_') a = '-';
        if (b == '_') b = '-';
        if (a != b)
            return false;
    }
    return true;
}

// Strip ISO 9660 file identifier decoration:
//   ";N"  version suffix  (e.g. "FOO.TXT;1" → "FOO.TXT")
//   "."   trailing dot    (e.g. "VMLINUZ_LTS." → "VMLINUZ_LTS")
static uint8_t strip_iso_suffix(const char *id, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        if (id[i] == ';') {
            len = i;
            break;
        }
    }
    if (len > 0 && id[len - 1] == '.')
        len--;
    return len;
}

// ── Primary Volume Descriptor ────────────────────────────────────────
//
// Offsets within a PVD sector (ECMA-119 sec. 8.4):
//   0     : type                (1 byte)
//   1-5   : "CD001"             (5 bytes)
//   156   : root directory record (34 bytes)

struct IsoDirRecord {
    const uint8_t *raw;

    uint8_t  length()     const { return static_cast<const volatile uint8_t *>(raw)[0]; }
    uint32_t extent_lba() const { return read_le32(raw + 2); }
    uint32_t data_size()  const { return read_le32(raw + 10); }
    uint8_t  flags()      const { return static_cast<const volatile uint8_t *>(raw)[25]; }
    bool     is_dir()     const { return flags() & DIR_FLAG_DIRECTORY; }
    uint8_t  id_len()     const { return static_cast<const volatile uint8_t *>(raw)[32]; }
    const char *id()      const { return reinterpret_cast<const char *>(raw + 33); }
};

// ── ISO validation ───────────────────────────────────────────────────

static bool check_signature(const volatile uint8_t *p, const char *sig, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] != static_cast<uint8_t>(sig[i]))
            return false;
    }
    return true;
}

bool iso_is_valid(const void *iso_base) {
    auto *pvd = reinterpret_cast<const volatile uint8_t *>(
        static_cast<const uint8_t *>(iso_base) + PVD_OFFSET);
    return pvd[0] == VD_TYPE_PRIMARY &&
           check_signature(pvd + 1, "CD001", 5);
}

// ── Directory search ─────────────────────────────────────────────────
//
// Walk directory records at the given LBA, looking for an entry whose
// identifier matches |name| (case-insensitive, version-suffix stripped).

static const uint8_t *find_in_directory(const void *iso_base,
                                        uint32_t dir_lba,
                                        uint32_t dir_size,
                                        const char *name,
                                        uint8_t name_len) {
    auto *base = static_cast<const uint8_t *>(iso_base);
    auto *dir  = base + static_cast<uint64_t>(dir_lba) * SECTOR_SIZE;
    uint32_t offset = 0;

    while (offset < dir_size) {
        auto *vdir = reinterpret_cast<const volatile uint8_t *>(dir);
        uint8_t rec_len = vdir[offset];

        if (rec_len == 0) {
            uint32_t next_sector = (offset / SECTOR_SIZE + 1) * SECTOR_SIZE;
            if (next_sector >= dir_size)
                break;
            offset = next_sector;
            continue;
        }

        IsoDirRecord rec{dir + offset};

        if (rec.id_len() == 1 &&
            (rec.id()[0] == '\0' || rec.id()[0] == '\1')) {
            offset += rec_len;
            continue;
        }

        uint8_t stripped = strip_iso_suffix(rec.id(), rec.id_len());
        if (name_eq(rec.id(), stripped, name, name_len))
            return dir + offset;

        offset += rec_len;
    }

    return nullptr;
}

// ── Path resolution ──────────────────────────────────────────────────
//
// Resolve a '/'-separated path like "boot/vmlinuz-lts" starting from
// the root directory record in the PVD.

bool iso_find_file(const void *iso_base, const char *path, IsoFile *out) {
    if (!iso_is_valid(iso_base))
        return false;

    auto *pvd = static_cast<const uint8_t *>(iso_base) + PVD_OFFSET;

    // Root directory record sits at PVD offset 156
    IsoDirRecord root{pvd + 156};
    uint32_t cur_lba  = root.extent_lba();
    uint32_t cur_size = root.data_size();

    // Walk each component of the path
    const char *p = path;
    while (*p == '/')
        p++;

    while (*p) {
        const char *seg = p;
        while (*p && *p != '/')
            p++;
        uint8_t seg_len = static_cast<uint8_t>(p - seg);
        while (*p == '/')
            p++;

        bool is_last = (*p == '\0');

        const uint8_t *found = find_in_directory(
            iso_base, cur_lba, cur_size, seg, seg_len);

        if (!found)
            return false;

        IsoDirRecord entry{found};

        if (is_last) {
            auto *base = static_cast<const uint8_t *>(iso_base);
            out->data = base + static_cast<uint64_t>(entry.extent_lba()) * SECTOR_SIZE;
            out->size = entry.data_size();
            return true;
        }

        if (!entry.is_dir())
            return false;

        cur_lba  = entry.extent_lba();
        cur_size = entry.data_size();
    }

    return false;
}
