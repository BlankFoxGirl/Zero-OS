#include "arch_interface.h"
#include "boot_info.h"
#include "string.h"

// ── PL011 UART (QEMU virt) ──────────────────────────────────────────

constexpr uintptr_t UART_BASE  = 0x09000000;
constexpr uintptr_t UART_DR    = UART_BASE + 0x000;
constexpr uintptr_t UART_FR    = UART_BASE + 0x018;
constexpr uint32_t  UART_FR_TXFF = (1 << 5);
constexpr uint32_t  UART_FR_RXFE = (1 << 4);

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

bool arch_serial_has_data() {
    return !(mmio_read32(UART_FR) & UART_FR_RXFE);
}

char arch_serial_getchar() {
    while (!arch_serial_has_data()) {}
    return static_cast<char>(mmio_read32(UART_DR) & 0xFF);
}

void arch_early_init() {
    // PL011 is pre-initialised by QEMU / firmware
}

[[noreturn]] void arch_halt() {
    for (;;)
        asm volatile("wfi");
}

// ── Entry point (called from boot_arm.S) ─────────────────────────────

constexpr uint64_t QEMU_VIRT_RAM_BASE = 0x40000000;
constexpr uint64_t QEMU_VIRT_RAM_SIZE = 128 * 1024 * 1024; // 128 MiB

extern "C" [[noreturn]]
void kernel_main(uint32_t dtb_addr) {
    UNUSED(dtb_addr);
    arch_early_init();

    BootInfo info{};
    info.arch_name           = "ARM (AArch32)";
    info.memory_region_count = 1;
    info.memory_regions[0]   = { QEMU_VIRT_RAM_BASE, QEMU_VIRT_RAM_SIZE,
                                 MEMORY_AVAILABLE };

    kernel_start(info);
}
