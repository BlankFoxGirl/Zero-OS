#include "vm.h"
#include "console.h"
#include "arch_interface.h"

#ifdef __aarch64__

// ── PL011 UART register offsets ──────────────────────────────────────

static constexpr uint64_t GUEST_UART_IPA = 0x09000000;
static constexpr uint64_t UART_REGION    = 0x00001000;  // 4 KiB register space

static constexpr uint32_t UART_DR   = 0x000;  // data register
static constexpr uint32_t UART_FR   = 0x018;  // flag register
static constexpr uint32_t UART_IBRD = 0x024;  // integer baud rate divisor
static constexpr uint32_t UART_FBRD = 0x028;  // fractional baud rate divisor
static constexpr uint32_t UART_LCR  = 0x02C;  // line control
static constexpr uint32_t UART_CR   = 0x030;  // control register
static constexpr uint32_t UART_IMSC = 0x038;  // interrupt mask set/clear
static constexpr uint32_t UART_ICR  = 0x044;  // interrupt clear

// ── Virtual UART state ───────────────────────────────────────────────

static struct {
    uint32_t cr;
    uint32_t lcr;
    uint32_t ibrd;
    uint32_t fbrd;
    uint32_t imsc;
} vuart;

// ── MMIO access handler ─────────────────────────────────────────────

bool vuart_mmio_access(uint64_t ipa, bool is_write, uint32_t width,
                       uint64_t *val) {
    if (ipa < GUEST_UART_IPA || ipa >= GUEST_UART_IPA + UART_REGION)
        return false;

    uint32_t offset = static_cast<uint32_t>(ipa - GUEST_UART_IPA);
    UNUSED(width);

    if (is_write) {
        uint32_t wval = static_cast<uint32_t>(*val);

        switch (offset) {
        case UART_DR:
            // Forward character to real serial output
            arch_serial_putchar(static_cast<char>(wval & 0xFF));
            break;
        case UART_CR:   vuart.cr   = wval; break;
        case UART_LCR:  vuart.lcr  = wval; break;
        case UART_IBRD: vuart.ibrd = wval; break;
        case UART_FBRD: vuart.fbrd = wval; break;
        case UART_IMSC: vuart.imsc = wval; break;
        case UART_ICR:  break;  // acknowledge, nothing to do
        default:
            break;
        }
    } else {
        uint32_t result = 0;

        switch (offset) {
        case UART_DR:
            result = 0;  // no input available
            break;
        case UART_FR:
            // TX FIFO never full, RX FIFO always empty
            // Bit 4 (RXFE) = 1, Bit 5 (TXFF) = 0, Bit 7 (TXFE) = 1
            result = (1u << 4) | (1u << 7);
            break;
        case UART_CR:   result = vuart.cr;   break;
        case UART_LCR:  result = vuart.lcr;  break;
        case UART_IBRD: result = vuart.ibrd; break;
        case UART_FBRD: result = vuart.fbrd; break;
        case UART_IMSC: result = vuart.imsc; break;
        default:
            break;
        }

        *val = result;
    }

    return true;
}

#endif /* __aarch64__ */
