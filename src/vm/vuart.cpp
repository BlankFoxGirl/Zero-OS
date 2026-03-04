#include "vm.h"
#include "console.h"
#include "arch_interface.h"

#ifdef __aarch64__

extern void vgic_inject_spi(uint32_t spi_num);

static constexpr uint32_t VUART_SPI = 1;  // SPI #1 → INTID 33 (matches DTB)

// ── Physical PL011 access (host UART provided by QEMU) ──────────────

static constexpr uintptr_t PHYS_UART_BASE = 0x09000000;

static inline uint32_t phys_uart_read(uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t *>(PHYS_UART_BASE + offset);
}

static inline void phys_uart_write(uint32_t offset, uint32_t val) {
    *reinterpret_cast<volatile uint32_t *>(PHYS_UART_BASE + offset) = val;
}

// ── PL011 UART register offsets ──────────────────────────────────────

static constexpr uint64_t GUEST_UART_IPA = 0x09000000;
static constexpr uint64_t UART_REGION    = 0x00001000;  // 4 KiB register space

static constexpr uint32_t UART_DR   = 0x000;  // data register
static constexpr uint32_t UART_FR   = 0x018;  // flag register
static constexpr uint32_t UART_IBRD = 0x024;  // integer baud rate divisor
static constexpr uint32_t UART_FBRD = 0x028;  // fractional baud rate divisor
static constexpr uint32_t UART_LCR  = 0x02C;  // line control
static constexpr uint32_t UART_CR   = 0x030;  // control register
static constexpr uint32_t UART_IFLS = 0x034;  // interrupt FIFO level select
static constexpr uint32_t UART_IMSC = 0x038;  // interrupt mask set/clear
static constexpr uint32_t UART_RIS  = 0x03C;  // raw interrupt status
static constexpr uint32_t UART_MIS  = 0x040;  // masked interrupt status
static constexpr uint32_t UART_ICR  = 0x044;  // interrupt clear
static constexpr uint32_t UART_DMACR = 0x048; // DMA control

static constexpr uint32_t FR_RXFE   = (1u << 4);  // RX FIFO empty
static constexpr uint32_t FR_TXFE   = (1u << 7);  // TX FIFO empty

// PrimeCell PeriphID / CellID registers (read-only)
// Values match QEMU's PL011 ARM variant so the AMBA bus driver can match.
static constexpr uint8_t PL011_ID[] = {
    0x11, 0x10, 0x14, 0x00,   // PeriphID0-3
    0x0D, 0xF0, 0x05, 0xB1,   // CellID0-3
};

// ── RX FIFO ─────────────────────────────────────────────────────────

static constexpr uint32_t RX_BUF_SIZE = 128;
static uint8_t  rx_buf[RX_BUF_SIZE];
static uint32_t rx_head;
static uint32_t rx_tail;
static uint32_t rx_count;

// ── Virtual UART state ───────────────────────────────────────────────

// PL011 interrupt bits
static constexpr uint32_t UART_INT_RX = (1u << 4);  // RXIS
static constexpr uint32_t UART_INT_TX = (1u << 5);  // TXIS

static struct {
    uint32_t cr;
    uint32_t lcr;
    uint32_t ibrd;
    uint32_t fbrd;
    uint32_t ifls;
    uint32_t imsc;
    uint32_t ris;   // raw interrupt status
} vuart;

static void vuart_update_irq() {
    vuart.ris |= UART_INT_TX;
    if (rx_count > 0)
        vuart.ris |= UART_INT_RX;

    uint32_t mis = vuart.ris & vuart.imsc;
    if (mis)
        vgic_inject_spi(VUART_SPI);
}

// Re-check UART interrupt condition — called before every guest entry
// so level-triggered semantics are honoured.
void vuart_check_irq() {
    vuart.ris |= UART_INT_TX;
    if (rx_count > 0)
        vuart.ris |= UART_INT_RX;
    uint32_t mis = vuart.ris & vuart.imsc;
    if (mis)
        vgic_inject_spi(VUART_SPI);
}

// ── Physical UART RX interrupt handler (called from vgic) ───────────

void vuart_handle_rx_irq() {
    while (!(phys_uart_read(UART_FR) & FR_RXFE)) {
        uint8_t ch = static_cast<uint8_t>(phys_uart_read(UART_DR) & 0xFF);
        if (rx_count < RX_BUF_SIZE) {
            rx_buf[rx_head] = ch;
            rx_head = (rx_head + 1) % RX_BUF_SIZE;
            rx_count++;
        }
    }

    if (rx_count > 0) {
        vuart.ris |= UART_INT_RX;
        vuart_update_irq();
    }
}

// ── Initialise the virtual UART and enable physical RX interrupts ────

void vuart_init() {
    rx_head = 0;
    rx_tail = 0;
    rx_count = 0;

    // Enable RX interrupt on the physical PL011 so we receive host input
    uint32_t imsc = phys_uart_read(UART_IMSC);
    imsc |= UART_INT_RX;
    phys_uart_write(UART_IMSC, imsc);
}

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
            arch_serial_putchar(static_cast<char>(wval & 0xFF));
            vuart_update_irq();
            break;
        case UART_CR:    vuart.cr   = wval; break;
        case UART_LCR:   vuart.lcr  = wval; break;
        case UART_IBRD:  vuart.ibrd = wval; break;
        case UART_FBRD:  vuart.fbrd = wval; break;
        case UART_IFLS:  vuart.ifls = wval; break;
        case UART_IMSC:
            vuart.imsc = wval;
            vuart_update_irq();
            break;
        case UART_ICR:
            vuart.ris &= ~wval;
            vuart.ris |= UART_INT_TX;
            if (rx_count > 0)
                vuart.ris |= UART_INT_RX;
            break;
        case UART_DMACR: break;
        default:
            break;
        }
    } else {
        uint32_t result = 0;

        if (offset >= 0xFE0 && offset <= 0xFFC) {
            uint32_t idx = (offset - 0xFE0) / 4;
            result = PL011_ID[idx];
        } else {
            switch (offset) {
            case UART_DR:
                if (rx_count > 0) {
                    result = rx_buf[rx_tail];
                    rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
                    rx_count--;
                    if (rx_count == 0)
                        vuart.ris &= ~UART_INT_RX;
                }
                break;
            case UART_FR:
                result = FR_TXFE;
                if (rx_count == 0)
                    result |= FR_RXFE;
                break;
            case UART_CR:   result = vuart.cr;   break;
            case UART_LCR:  result = vuart.lcr;  break;
            case UART_IBRD: result = vuart.ibrd; break;
            case UART_FBRD: result = vuart.fbrd; break;
            case UART_IFLS: result = vuart.ifls; break;
            case UART_IMSC: result = vuart.imsc; break;
            case UART_RIS:  result = vuart.ris;  break;
            case UART_MIS:  result = vuart.ris & vuart.imsc; break;
            default:
                break;
            }
        }

        *val = result;
    }

    return true;
}

#endif /* __aarch64__ */
