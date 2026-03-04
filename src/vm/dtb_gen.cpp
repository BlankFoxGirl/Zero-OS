#include "vm.h"
#include "string.h"
#include "console.h"

#ifdef __aarch64__

// ── Big-endian helpers (FDT is always big-endian) ────────────────────

static inline uint32_t cpu_to_be32(uint32_t v) {
    return __builtin_bswap32(v);
}
static inline uint64_t cpu_to_be64(uint64_t v) {
    return __builtin_bswap64(v);
}

// ── FDT constants ────────────────────────────────────────────────────

static constexpr uint32_t FDT_MAGIC       = 0xD00DFEED;
static constexpr uint32_t FDT_VERSION     = 17;
static constexpr uint32_t FDT_LAST_COMPAT = 16;

static constexpr uint32_t FDT_BEGIN_NODE  = 1;
static constexpr uint32_t FDT_END_NODE    = 2;
static constexpr uint32_t FDT_PROP        = 3;
static constexpr uint32_t FDT_END         = 9;

static constexpr uint32_t FDT_HEADER_SIZE = 40;

// ── Simple FDT builder ───────────────────────────────────────────────

static constexpr uint32_t STRUCT_MAX = 4096;
static constexpr uint32_t STRINGS_MAX = 512;

static uint8_t  s_struct[STRUCT_MAX];
static uint32_t s_struct_len;
static char     s_strings[STRINGS_MAX];
static uint32_t s_strings_len;

static void fdt_put32(uint32_t val) {
    if (s_struct_len + 4 > STRUCT_MAX) return;
    auto *p = reinterpret_cast<uint32_t *>(s_struct + s_struct_len);
    *p = cpu_to_be32(val);
    s_struct_len += 4;
}

static void fdt_put_data(const void *data, uint32_t len) {
    if (s_struct_len + len > STRUCT_MAX) return;
    memcpy(s_struct + s_struct_len, data, len);
    s_struct_len += len;
    // Pad to 4-byte alignment
    while (s_struct_len & 3) {
        s_struct[s_struct_len++] = 0;
    }
}

static uint32_t fdt_add_string(const char *name) {
    uint32_t name_len = static_cast<uint32_t>(strlen(name)) + 1;
    // Check if already in strings table
    for (uint32_t i = 0; i + name_len <= s_strings_len; i++) {
        if (memcmp(s_strings + i, name, name_len) == 0)
            return i;
    }
    if (s_strings_len + name_len > STRINGS_MAX)
        return 0;
    uint32_t off = s_strings_len;
    memcpy(s_strings + s_strings_len, name, name_len);
    s_strings_len += name_len;
    return off;
}

static void fdt_begin_node(const char *name) {
    fdt_put32(FDT_BEGIN_NODE);
    uint32_t len = static_cast<uint32_t>(strlen(name)) + 1;
    fdt_put_data(name, len);
}

static void fdt_end_node() {
    fdt_put32(FDT_END_NODE);
}

static void fdt_prop_raw(const char *name, const void *data, uint32_t len) {
    fdt_put32(FDT_PROP);
    fdt_put32(len);
    fdt_put32(fdt_add_string(name));
    if (len > 0)
        fdt_put_data(data, len);
}

static void fdt_prop_u32(const char *name, uint32_t val) {
    uint32_t be = cpu_to_be32(val);
    fdt_prop_raw(name, &be, 4);
}

static void fdt_prop_u64(const char *name, uint64_t val) {
    uint64_t be = cpu_to_be64(val);
    fdt_prop_raw(name, &be, 8);
}

static void fdt_prop_str(const char *name, const char *val) {
    fdt_prop_raw(name, val, static_cast<uint32_t>(strlen(val)) + 1);
}

static void fdt_prop_strs(const char *name, const char *strs,
                          uint32_t total_len) {
    fdt_prop_raw(name, strs, total_len);
}

static void fdt_prop_empty(const char *name) {
    fdt_prop_raw(name, nullptr, 0);
}

static void fdt_prop_cells(const char *name, const uint32_t *cells,
                           uint32_t count) {
    uint32_t buf[32];
    if (count > 32) count = 32;
    for (uint32_t i = 0; i < count; i++)
        buf[i] = cpu_to_be32(cells[i]);
    fdt_prop_raw(name, buf, count * 4);
}

// ── Build the complete VM device tree ────────────────────────────────

static constexpr uint32_t PHANDLE_INTC   = 1;
static constexpr uint32_t PHANDLE_CLK    = 2;

uint32_t dtb_generate(void *out_buf, uint32_t buf_size,
                      uint64_t ram_base, uint64_t ram_size,
                      uint64_t initrd_start, uint64_t initrd_end) {
    s_struct_len = 0;
    s_strings_len = 0;

    // ── root node ──
    fdt_begin_node("");
    fdt_prop_u32("#address-cells", 2);
    fdt_prop_u32("#size-cells", 2);
    fdt_prop_str("compatible", "linux,dummy-virt");
    fdt_prop_str("model", "ZeroOS Virtual Machine");
    fdt_prop_u32("interrupt-parent", PHANDLE_INTC);

    // ── cpus ──
    fdt_begin_node("cpus");
    fdt_prop_u32("#address-cells", 1);
    fdt_prop_u32("#size-cells", 0);

    fdt_begin_node("cpu@0");
    fdt_prop_str("device_type", "cpu");
    fdt_prop_str("compatible", "arm,cortex-a57");
    fdt_prop_u32("reg", 0);
    fdt_end_node(); // cpu@0

    fdt_end_node(); // cpus

    // ── memory ──
    fdt_begin_node("memory@40000000");
    fdt_prop_str("device_type", "memory");
    {
        uint32_t reg[] = {
            static_cast<uint32_t>(ram_base >> 32),
            static_cast<uint32_t>(ram_base),
            static_cast<uint32_t>(ram_size >> 32),
            static_cast<uint32_t>(ram_size),
        };
        fdt_prop_cells("reg", reg, 4);
    }
    fdt_end_node(); // memory

    // ── interrupt controller (GICv2) ──
    fdt_begin_node("intc@8000000");
    fdt_prop_str("compatible", "arm,cortex-a15-gic");
    fdt_prop_u32("#interrupt-cells", 3);
    fdt_prop_empty("interrupt-controller");
    fdt_prop_u32("phandle", PHANDLE_INTC);
    {
        uint32_t reg[] = {
            0, 0x08000000, 0, 0x10000,   // GICD
            0, 0x08010000, 0, 0x10000,   // GICC
        };
        fdt_prop_cells("reg", reg, 8);
    }
    fdt_end_node(); // intc

    // ── timer ──
    fdt_begin_node("timer");
    fdt_prop_str("compatible", "arm,armv8-timer");
    {
        // <type irq flags> — PPI interrupts
        // type=1 (PPI), flags=0xf08 (level-low, all CPUs)
        uint32_t interrupts[] = {
            1, 13, 0xf08,   // secure phys
            1, 14, 0xf08,   // non-secure phys
            1, 11, 0xf08,   // virtual
            1, 10, 0xf08,   // hypervisor
        };
        fdt_prop_cells("interrupts", interrupts, 12);
    }
    fdt_prop_empty("always-on");
    fdt_end_node(); // timer

    // ── UART (PL011) ──
    fdt_begin_node("pl011@9000000");
    {
        const char compat[] = "arm,pl011\0arm,primecell";
        fdt_prop_strs("compatible", compat, sizeof(compat));
    }
    {
        uint32_t reg[] = { 0, 0x09000000, 0, 0x1000 };
        fdt_prop_cells("reg", reg, 4);
    }
    {
        uint32_t clocks[] = { PHANDLE_CLK, PHANDLE_CLK };
        fdt_prop_cells("clocks", clocks, 2);
    }
    {
        const char names[] = "uartclk\0apb_pclk";
        fdt_prop_strs("clock-names", names, sizeof(names));
    }
    fdt_end_node(); // pl011

    // ── fixed clock (for UART) ──
    fdt_begin_node("apb-pclk");
    fdt_prop_str("compatible", "fixed-clock");
    fdt_prop_u32("#clock-cells", 0);
    fdt_prop_u32("clock-frequency", 24000000);
    fdt_prop_str("clock-output-names", "apb_pclk");
    fdt_prop_u32("phandle", PHANDLE_CLK);
    fdt_end_node(); // apb-pclk

    // ── chosen ──
    fdt_begin_node("chosen");
    fdt_prop_str("bootargs",
                 "console=ttyAMA0 earlycon=pl011,0x09000000");

    if (initrd_start && initrd_end > initrd_start) {
        fdt_prop_u64("linux,initrd-start", initrd_start);
        fdt_prop_u64("linux,initrd-end", initrd_end);
    }
    fdt_end_node(); // chosen

    fdt_end_node(); // root
    fdt_put32(FDT_END);

    // ── Assemble the final FDT blob ──────────────────────────────────

    // Memory reservation block: single empty entry (16 bytes of zeroes)
    static constexpr uint32_t RSVMAP_SIZE = 16;

    uint32_t off_rsvmap  = FDT_HEADER_SIZE;
    uint32_t off_struct  = off_rsvmap + RSVMAP_SIZE;
    uint32_t off_strings = off_struct + s_struct_len;
    uint32_t total_size  = off_strings + s_strings_len;

    // Align total size to 4 bytes
    total_size = (total_size + 3) & ~3u;

    if (total_size > buf_size) {
        kprintf("dtb: buffer too small (%u needed, %u available)\n",
                total_size, buf_size);
        return 0;
    }

    auto *buf = reinterpret_cast<uint8_t *>(out_buf);
    memset(buf, 0, total_size);

    // Header
    auto *hdr = reinterpret_cast<uint32_t *>(buf);
    hdr[0] = cpu_to_be32(FDT_MAGIC);
    hdr[1] = cpu_to_be32(total_size);
    hdr[2] = cpu_to_be32(off_struct);
    hdr[3] = cpu_to_be32(off_strings);
    hdr[4] = cpu_to_be32(off_rsvmap);
    hdr[5] = cpu_to_be32(FDT_VERSION);
    hdr[6] = cpu_to_be32(FDT_LAST_COMPAT);
    hdr[7] = cpu_to_be32(0);  // boot_cpuid_phys
    hdr[8] = cpu_to_be32(s_strings_len);
    hdr[9] = cpu_to_be32(s_struct_len);

    // Reservation map (empty: 16 bytes of zeroes, already zeroed)

    // Structure block
    memcpy(buf + off_struct, s_struct, s_struct_len);

    // Strings block
    memcpy(buf + off_strings, s_strings, s_strings_len);

    kprintf("dtb: generated %u bytes\n", total_size);
    return total_size;
}

#endif /* __aarch64__ */
