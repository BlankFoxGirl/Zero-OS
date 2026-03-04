#include "arch_interface.h"
#include "boot_info.h"
#include "console.h"
#include "string.h"

// ── PL011 UART (QEMU virt) ──────────────────────────────────────────

constexpr uintptr_t UART_BASE  = 0x09000000;
constexpr uintptr_t UART_DR    = UART_BASE + 0x000;
constexpr uintptr_t UART_FR    = UART_BASE + 0x018;
constexpr uint32_t  UART_FR_TXFF = (1 << 5);

static inline void mmio_write32(uintptr_t addr, uint32_t val) {
    *reinterpret_cast<volatile uint32_t *>(addr) = val;
}

static inline uint32_t mmio_read32(uintptr_t addr) {
    return *reinterpret_cast<volatile uint32_t *>(addr);
}

// ── arch_interface implementation ────────────────────────────────────

void arch_serial_putchar(char c) {
    while (mmio_read32(UART_FR) & UART_FR_TXFF) {}
    mmio_write32(UART_DR, static_cast<uint32_t>(c));
}

void arch_serial_write(const char *str, size_t len) {
    for (size_t i = 0; i < len; i++)
        arch_serial_putchar(str[i]);
}

void arch_early_init() {
    // PL011 is pre-initialised by QEMU / firmware
}

[[noreturn]] void arch_halt() {
    for (;;)
        asm volatile("wfi");
}

// ── EL2 exception stub (called from vectors.S) ──────────────────────

static const char *vector_name(uint64_t idx) {
    static const char *names[] = {
        "CurEL_SP0 Sync",  "CurEL_SP0 IRQ",
        "CurEL_SP0 FIQ",   "CurEL_SP0 SError",
        "CurEL_SPx Sync",  "CurEL_SPx IRQ",
        "CurEL_SPx FIQ",   "CurEL_SPx SError",
        "LowEL_64 Sync",   "LowEL_64 IRQ",
        "LowEL_64 FIQ",    "LowEL_64 SError",
        "LowEL_32 Sync",   "LowEL_32 IRQ",
        "LowEL_32 FIQ",    "LowEL_32 SError",
    };
    if (idx < 16) return names[idx];
    return "Unknown";
}

extern "C"
void hyp_exception_handler(uint64_t type, uint64_t esr,
                           uint64_t elr, uint64_t far) {
    uint32_t ec  = static_cast<uint32_t>((esr >> 26) & 0x3F);
    uint32_t iss = static_cast<uint32_t>(esr & 0x1FFFFFF);

    kprintf("\n*** EL2 EXCEPTION ***\n");
    kprintf("  Vector : %s (#%llu)\n", vector_name(type),
            (unsigned long long)type);
    kprintf("  ESR_EL2: 0x%08llx  EC=0x%02x  ISS=0x%07x\n",
            (unsigned long long)esr, ec, iss);
    kprintf("  ELR_EL2: 0x%016llx\n", (unsigned long long)elr);
    kprintf("  FAR_EL2: 0x%016llx\n", (unsigned long long)far);

    arch_halt();
}

// ── Entry point (called from boot_aarch64.S) ─────────────────────────

constexpr uint64_t QEMU_VIRT_RAM_BASE = 0x40000000;
constexpr uint64_t HYP_RAM_SIZE       = 128ULL * 1024 * 1024;

static uint64_t read_current_el() {
    uint64_t val;
    asm volatile("mrs %0, CurrentEL" : "=r"(val));
    return (val >> 2) & 0x3;
}

extern "C" [[noreturn]]
void kernel_main(uint64_t dtb_addr) {
    UNUSED(dtb_addr);
    arch_early_init();

    uint64_t el = read_current_el();
    kprintf("[boot] Running at EL%llu\n", (unsigned long long)el);

    if (el != 2) {
        kprintf("FATAL: ZeroOS requires EL2 for hypervisor operation.\n");
        arch_halt();
    }

    BootInfo info{};
    info.arch_name           = "AArch64 (EL2 Hypervisor)";
    info.memory_region_count = 1;
    info.memory_regions[0]   = { QEMU_VIRT_RAM_BASE, HYP_RAM_SIZE,
                                 MEMORY_AVAILABLE };

    kernel_start(info);
}
