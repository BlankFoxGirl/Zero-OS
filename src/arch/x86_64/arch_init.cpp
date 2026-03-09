#include "arch_interface.h"
#include "boot_info.h"
#include "multiboot2.h"
#include "console.h"
#include "fb_console.h"
#include "string.h"
#include "x86/msr.h"

#ifdef __x86_64__
#include "x86/efi.h"
#endif

// ── Dynamic page-table mapping ───────────────────────────────────────
//
// The boot code identity-maps 0–4 GiB.  GPU framebuffers on UEFI
// systems can live above 4 GiB, so we dynamically add 2 MiB-page
// mappings for any physical region the kernel needs at runtime.
// 128 spare PDs = up to 132 GiB total identity mapping.

extern "C" uint64_t pml4[];
extern "C" uint64_t pdpt[];
extern "C" void switch_to_kernel_gdt();

alignas(4096) static uint64_t spare_pds[128][512];
static uint32_t spare_pd_used = 0;

extern "C" bool ensure_physical_mapped(uint64_t phys_start, uint64_t size) {
    uint64_t phys_end = phys_start + size;
    if (phys_end <= 0x100000000ULL)
        return true;

    uint64_t region = phys_start & ~0x3FFFFFFFULL;
    while (region < phys_end) {
        uint32_t idx = static_cast<uint32_t>(region >> 30);
        if (idx >= 512)
            return false;

        if (!(pdpt[idx] & 1)) {
            if (spare_pd_used >= 128)
                return false;

            uint64_t *new_pd = spare_pds[spare_pd_used++];
            uint64_t base = static_cast<uint64_t>(idx) << 30;
            for (uint32_t i = 0; i < 512; i++)
                new_pd[i] = (base + i * 0x200000ULL) | 0x83;

            pdpt[idx] = reinterpret_cast<uint64_t>(new_pd) | 0x03;
            asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3"
                         ::: "rax", "memory");
        }
        region += 0x40000000ULL;
    }
    return true;
}

// ── Write-Combining for framebuffer ──────────────────────────────────
//
// UEFI framebuffers default to Uncacheable (UC), making every pixel
// write stall on the PCI bus.  We reprogram PAT entry 1 to WC
// (Write-Combining) and flip the PWT bit in the relevant 2 MiB page
// table entries so framebuffer writes are buffered and flushed in
// bursts — typically a 10–100× speedup.

// MSR helpers and constants now in include/x86/msr.h

static void setup_pat_wc() {
    uint64_t pat = x86_rdmsr(MSR_PAT);
    pat &= ~(0x07ULL << 8);
    pat |= (0x01ULL << 8);   // PAT1 = WC (Write-Combining)
    x86_wrmsr(MSR_PAT, pat);
}

static void set_wc_for_range(uint64_t phys_start, uint64_t size) {
    uint64_t phys_end = phys_start + size;
    uint64_t page = phys_start & ~0x1FFFFFULL;

    while (page < phys_end) {
        uint32_t pdpt_idx = static_cast<uint32_t>(page >> 30);
        uint32_t pd_idx   = static_cast<uint32_t>((page >> 21) & 0x1FF);

        if (pdpt_idx < 512 && (pdpt[pdpt_idx] & 1)) {
            uint64_t pd_phys = pdpt[pdpt_idx] & ~0xFFFULL;
            auto *pd_entries = reinterpret_cast<uint64_t *>(pd_phys);
            pd_entries[pd_idx] |= (1ULL << 3); // PWT → selects PAT1 = WC
        }
        page += 0x200000ULL;
    }

    asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
}

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

static bool serial_input_ok = false;

static bool serial_loopback_test() {
    uint8_t orig_mcr = x86_inb(COM1_MODEM_CTRL);
    x86_outb(COM1_MODEM_CTRL, 0x1E);   // loopback + OUT2/OUT1/RTS
    x86_outb(COM1_DATA, 0xAE);

    for (volatile int i = 0; i < 10000; i++) {}

    bool ok = (x86_inb(COM1_LINE_STAT) & 0x01) &&
              (x86_inb(COM1_DATA) == 0xAE);

    x86_outb(COM1_MODEM_CTRL, orig_mcr);
    return ok;
}

static void serial_init() {
    x86_outb(COM1_INT_EN,     0x00);   // disable interrupts
    x86_outb(COM1_LINE_CTRL,  0x80);   // enable DLAB
    x86_outb(COM1 + 0,        0x01);   // divisor lo  (115200 baud)
    x86_outb(COM1 + 1,        0x00);   // divisor hi
    x86_outb(COM1_LINE_CTRL,  0x03);   // 8N1
    x86_outb(COM1_FIFO,       0xC7);   // enable+clear FIFO, 14-byte threshold
    x86_outb(COM1_MODEM_CTRL, 0x03);   // RTS + DTR

    if (!serial_loopback_test()) {
        serial_input_ok = false;
        return;
    }

    for (int i = 0; i < 16; i++) {
        if (!(x86_inb(COM1_LINE_STAT) & 0x01))
            break;
        (void)x86_inb(COM1_DATA);
    }

    for (volatile int i = 0; i < 2000000; i++) {}

    if (x86_inb(COM1_LINE_STAT) & 0x01) {
        for (int i = 0; i < 16; i++) {
            if (!(x86_inb(COM1_LINE_STAT) & 0x01))
                break;
            (void)x86_inb(COM1_DATA);
        }
        serial_input_ok = false;
    } else {
        serial_input_ok = true;
    }
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

bool arch_serial_has_data() {
    return x86_inb(COM1_LINE_STAT) & 0x01;
}

char arch_serial_getchar() {
    while (!arch_serial_has_data()) {}
    return static_cast<char>(x86_inb(COM1_DATA));
}

// ── PS/2 keyboard (scancode set 1) ───────────────────────────────────

constexpr uint16_t KBD_DATA   = 0x60;
constexpr uint16_t KBD_STATUS = 0x64;

static const char scancode_to_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ',
};

static bool kbd_has_data() {
    return x86_inb(KBD_STATUS) & 0x01;
}

static char kbd_try_read() {
    uint8_t sc = x86_inb(KBD_DATA);
    if (sc & 0x80)
        return 0;       // key release, ignore
    if (sc < sizeof(scancode_to_ascii))
        return scancode_to_ascii[sc];
    return 0;
}

// ── Console input (serial + keyboard) ────────────────────────────────

bool arch_console_has_input() {
    if (serial_input_ok && arch_serial_has_data())
        return true;
    return kbd_has_data();
}

char arch_console_getchar() {
    for (;;) {
        if (serial_input_ok && arch_serial_has_data())
            return arch_serial_getchar();
        if (kbd_has_data()) {
            char c = kbd_try_read();
            if (c)
                return c;
        }
    }
}

void x86_64_idt_init();

void arch_early_init() {
    serial_init();
    setup_pat_wc();
    x86_64_idt_init();
}

[[noreturn]] void arch_halt() {
    for (;;)
        asm volatile("cli; hlt");
}

// ── PS/2 Ctrl+Alt+Delete detection ───────────────────────────────────
// Scancode set 1: Ctrl=0x1D, Alt=0x38, Delete=0x53
// Break codes have bit 7 set (e.g. Ctrl release = 0x9D).

static bool s_ctrl_held = false;
static bool s_alt_held  = false;

bool arch_poll_ctrl_alt_del() {
    while (kbd_has_data()) {
        uint8_t sc = x86_inb(KBD_DATA);
        switch (sc) {
        case 0x1D: s_ctrl_held = true;  break;
        case 0x9D: s_ctrl_held = false; break;
        case 0x38: s_alt_held  = true;  break;
        case 0xB8: s_alt_held  = false; break;
        case 0x53:
            if (s_ctrl_held && s_alt_held)
                return true;
            break;
        }
    }
    return false;
}

[[noreturn]] void arch_reboot() {
    // Pulse the CPU reset line via the 8042 keyboard controller
    while (x86_inb(KBD_STATUS) & 0x02) {}
    x86_outb(KBD_STATUS, 0xFE);

    // Fallback: if the 8042 command didn't reset, just halt
    for (;;)
        asm volatile("cli; hlt");
}

// ── Multiboot2 parsing ──────────────────────────────────────────────

static void copy_module_name(char *dst, const char *src, uint32_t max_len) {
    uint32_t i = 0;
    while (i < max_len - 1 && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

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
        } else if (tag->type == MB2_TAG_FRAMEBUFFER) {
            auto *fb = reinterpret_cast<Multiboot2FramebufferTag *>(tag);
            info.framebuffer.addr      = fb->addr;
            info.framebuffer.pitch     = fb->pitch;
            info.framebuffer.width     = fb->width;
            info.framebuffer.height    = fb->height;
            info.framebuffer.bpp       = fb->bpp;
            info.framebuffer.available = true;
        } else if (tag->type == MB2_TAG_MODULE &&
                   info.module_count < MAX_BOOT_MODULES) {
            auto *mod = reinterpret_cast<Multiboot2ModuleTag *>(tag);
            auto &m = info.modules[info.module_count++];
            m.hpa  = mod->mod_start;
            m.size = mod->mod_end - mod->mod_start;
            copy_module_name(m.name, mod->cmdline(), BOOT_MODULE_NAME_LEN);
        } else if (tag->type == MB2_TAG_EFI_ST64) {
            auto *efi_st = reinterpret_cast<uintptr_t *>(
                reinterpret_cast<uintptr_t>(tag) + 8);
            info.efi.system_table = reinterpret_cast<void *>(*efi_st);
        } else if (tag->type == MB2_TAG_EFI_BS) {
            info.efi.bs_active = true;
        } else if (tag->type == MB2_TAG_EFI_IMG_HANDLE64) {
            auto *efi_ih = reinterpret_cast<uintptr_t *>(
                reinterpret_cast<uintptr_t>(tag) + 8);
            info.efi.image_handle = reinterpret_cast<void *>(*efi_ih);
        }

        uintptr_t next = reinterpret_cast<uintptr_t>(tag) + tag->size;
        next = ALIGN_UP(next, 8);
        tag  = reinterpret_cast<Multiboot2Tag *>(next);
    }
}

// ── Multiboot1 parsing (used by QEMU -kernel) ──────────────────────

static void parse_multiboot1(uint32_t mb_info_addr, BootInfo &info) {
    auto *mbi = reinterpret_cast<Multiboot1Info *>(
        static_cast<uintptr_t>(mb_info_addr));

    if (!(mbi->flags & MB1_FLAG_MMAP))
        return;

    auto *entry = reinterpret_cast<Multiboot1MmapEntry *>(
        static_cast<uintptr_t>(mbi->mmap_addr));
    uintptr_t end = mbi->mmap_addr + mbi->mmap_length;

    while (reinterpret_cast<uintptr_t>(entry) < end &&
           info.memory_region_count < MAX_MEMORY_REGIONS) {
        auto &r = info.memory_regions[info.memory_region_count++];
        r.base   = entry->base_addr;
        r.length = entry->length;
        r.type   = entry->type;

        entry = reinterpret_cast<Multiboot1MmapEntry *>(
            reinterpret_cast<uintptr_t>(entry) + entry->size + 4);
    }
}

// ── Derive ram_base / total_ram from parsed memory regions ──────────

static void derive_ram_totals(BootInfo &info) {
    uint64_t lowest_base = UINT64_MAX;
    uint64_t total = 0;
    for (uint32_t i = 0; i < info.memory_region_count; i++) {
        if (info.memory_regions[i].type != MEMORY_AVAILABLE)
            continue;
        total += info.memory_regions[i].length;
        if (info.memory_regions[i].base < lowest_base)
            lowest_base = info.memory_regions[i].base;
    }
    info.ram_base  = (lowest_base == UINT64_MAX) ? 0 : lowest_base;
    info.total_ram = total;
}

// ── Entry point (called from boot.S) ─────────────────────────────────

extern "C" [[noreturn]]
void kernel_main(uint32_t mb_magic, uint32_t mb_info_addr) {
    // Serial + PAT are safe with any GDT (only touch I/O ports and MSRs).
    // IDT and framebuffer mapping are deferred until our GDT + CR3 are live.
    serial_init();
    setup_pat_wc();

    BootInfo info{};
    info.arch_name           = "x86_64";
    info.memory_region_count = 0;

    if (mb_magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        parse_multiboot2(mb_info_addr, info);
    } else if (mb_magic == MULTIBOOT1_BOOTLOADER_MAGIC) {
        parse_multiboot1(mb_info_addr, info);
    } else {
        kprintf("WARNING: unknown multiboot magic 0x%x\n", mb_magic);
    }

    // ── EFI image loading ────────────────────────────────────────
    // _start_efi64 kept the firmware's GDT, segments, and CR3 intact
    // so that EFI protocol calls work.  We must finish all EFI work
    // BEFORE loading our own GDT/IDT/CR3.
#ifdef __x86_64__
    if (info.efi.bs_active && info.efi.system_table) {
        // Bring up the framebuffer early so the image selection menu
        // is visible on screen.  During EFI boot the firmware's page
        // tables already identity-map the framebuffer MMIO region.
        // WC optimisation is applied later after switching to our CR3.
        if (info.framebuffer.available)
            fb_init(info.framebuffer);

        kprintf("efi: Boot Services active — loading guest image via UEFI\n");

        // GRUB doesn't provide the Multiboot2 memory map tag when EFI BS
        // are preserved.  Get the memory map directly from EFI firmware.
        efi_populate_memory_map(info.efi.system_table, &info);
        derive_ram_totals(info);

        MemoryLayout layout = compute_memory_layout(info.ram_base,
                                                    info.total_ram);
        EfiLoadResult efi_result{};
        if (efi_load_guest_image(info.efi.system_table,
                                 info.efi.image_handle,
                                 layout.ramdisk_hpa,
                                 layout.ramdisk_size,
                                 &efi_result)) {
            info.efi_image.loaded = true;
            info.efi_image.hpa    = efi_result.hpa;
            info.efi_image.size   = efi_result.size;
            memcpy(info.efi_image.name, efi_result.name,
                   sizeof(info.efi_image.name));
        }

        efi_exit_boot_services(info.efi.system_table,
                               info.efi.image_handle);
        info.efi.bs_active = false;

        // Pre-map the framebuffer in our kernel page tables BEFORE
        // switching CR3.  On UEFI systems the FB often lives above
        // 4 GiB; without this mapping the first kprintf after the
        // CR3 switch triple-faults (page fault with no IDT loaded).
        if (info.framebuffer.available) {
            uint64_t fb_size = static_cast<uint64_t>(info.framebuffer.pitch)
                             * info.framebuffer.height;
            ensure_physical_mapped(info.framebuffer.addr, fb_size);
        }

        // Firmware is gone — switch to our GDT, segments, and page tables.
        switch_to_kernel_gdt();

        asm volatile("mov %0, %%cr3" :: "r"(reinterpret_cast<uint64_t>(pml4))
                     : "memory");

        // IDT must be live before any kprintf — a stray fault with
        // no IDT cascades into a triple fault and CPU reset.
        x86_64_idt_init();

        // Apply write-combining for framebuffer performance now that
        // our page tables are active.
        if (info.framebuffer.available) {
            uint64_t fb_size = static_cast<uint64_t>(info.framebuffer.pitch)
                             * info.framebuffer.height;
            set_wc_for_range(info.framebuffer.addr, fb_size);
        }

        kprintf("efi: switched to kernel GDT + page tables\n");
    } else
#endif
    {
        // Non-EFI path: memory map came from Multiboot2 MMAP tag.
        derive_ram_totals(info);
    }

    // Our GDT is now active (loaded by _start for BIOS, or above for EFI).
    x86_64_idt_init();

    if (info.framebuffer.available) {
        uint64_t fb_size = static_cast<uint64_t>(info.framebuffer.pitch)
                         * info.framebuffer.height;
        if (ensure_physical_mapped(info.framebuffer.addr, fb_size)) {
            set_wc_for_range(info.framebuffer.addr, fb_size);
            if (!fb_available())
                fb_init(info.framebuffer);
        }
    }

    kernel_start(info);
}
