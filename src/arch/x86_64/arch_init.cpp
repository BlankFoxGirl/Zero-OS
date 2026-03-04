#include "arch_interface.h"
#include "boot_info.h"
#include "multiboot2.h"
#include "console.h"
#include "string.h"

// ── x86 I/O port helpers ─────────────────────────────────────────────

static inline void x86_outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t x86_inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ── COM1 serial (8250 / 16550) ───────────────────────────────────────

constexpr uint16_t COM1            = 0x3F8;
constexpr uint16_t COM1_DATA       = COM1 + 0;
constexpr uint16_t COM1_INT_EN     = COM1 + 1;
constexpr uint16_t COM1_FIFO       = COM1 + 2;
constexpr uint16_t COM1_LINE_CTRL  = COM1 + 3;
constexpr uint16_t COM1_MODEM_CTRL = COM1 + 4;
constexpr uint16_t COM1_LINE_STAT  = COM1 + 5;

static void serial_init() {
    x86_outb(COM1_INT_EN,     0x00);   // disable interrupts
    x86_outb(COM1_LINE_CTRL,  0x80);   // enable DLAB
    x86_outb(COM1 + 0,        0x01);   // divisor lo  (115200 baud)
    x86_outb(COM1 + 1,        0x00);   // divisor hi
    x86_outb(COM1_LINE_CTRL,  0x03);   // 8N1
    x86_outb(COM1_FIFO,       0xC7);   // enable FIFO, 14-byte threshold
    x86_outb(COM1_MODEM_CTRL, 0x03);   // RTS + DTR
}

// ── arch_interface implementation ────────────────────────────────────

void arch_serial_putchar(char c) {
    while (!(x86_inb(COM1_LINE_STAT) & 0x20)) {}
    x86_outb(COM1_DATA, static_cast<uint8_t>(c));
}

void arch_serial_write(const char *str, size_t len) {
    for (size_t i = 0; i < len; i++)
        arch_serial_putchar(str[i]);
}

void arch_early_init() {
    serial_init();
}

[[noreturn]] void arch_halt() {
    for (;;)
        asm volatile("cli; hlt");
}

// ── Multiboot2 parsing ──────────────────────────────────────────────

static void parse_multiboot2(uint32_t mb_info_addr, BootInfo &info) {
    auto *mb = reinterpret_cast<Multiboot2FixedPart *>(
        static_cast<uintptr_t>(mb_info_addr));
    auto *tag = reinterpret_cast<Multiboot2Tag *>(
        reinterpret_cast<uintptr_t>(mb) + sizeof(Multiboot2FixedPart));
    auto *end = reinterpret_cast<void *>(
        reinterpret_cast<uintptr_t>(mb) + mb->total_size);

    while (reinterpret_cast<void *>(tag) < end && tag->type != MB2_TAG_END) {
        if (tag->type == MB2_TAG_MMAP) {
            auto *mmap = reinterpret_cast<Multiboot2MmapTag *>(tag);
            auto *entry = reinterpret_cast<Multiboot2MmapEntry *>(
                reinterpret_cast<uintptr_t>(mmap) + sizeof(Multiboot2MmapTag));
            auto *mmap_end = reinterpret_cast<Multiboot2MmapEntry *>(
                reinterpret_cast<uintptr_t>(mmap) + mmap->size);

            while (entry < mmap_end &&
                   info.memory_region_count < MAX_MEMORY_REGIONS) {
                auto &r = info.memory_regions[info.memory_region_count++];
                r.base   = entry->base_addr;
                r.length = entry->length;
                r.type   = entry->type;

                entry = reinterpret_cast<Multiboot2MmapEntry *>(
                    reinterpret_cast<uintptr_t>(entry) + mmap->entry_size);
            }
        }

        uintptr_t next = reinterpret_cast<uintptr_t>(tag) + tag->size;
        next = ALIGN_UP(next, 8);
        tag  = reinterpret_cast<Multiboot2Tag *>(next);
    }
}

// ── Entry point (called from boot.S) ─────────────────────────────────

extern "C" [[noreturn]]
void kernel_main(uint32_t mb_magic, uint32_t mb_info_addr) {
    arch_early_init();

    BootInfo info{};
    info.arch_name           = "x86_64";
    info.memory_region_count = 0;

    if (mb_magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        parse_multiboot2(mb_info_addr, info);
    } else {
        kprintf("WARNING: bad Multiboot2 magic 0x%x (expected 0x%x)\n",
                mb_magic, MULTIBOOT2_BOOTLOADER_MAGIC);
    }

    kernel_start(info);
}
