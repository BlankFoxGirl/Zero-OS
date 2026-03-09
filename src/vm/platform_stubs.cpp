#include "x86/platform.h"
#include "console.h"

#ifdef __x86_64__

// ── PIT (i8254) ──────────────────────────────────────────────────────

static uint16_t pit_counter = 0xFFFF;
static bool     pit_latched;
static bool     pit_read_lo;

void platform_stubs_init() {
    pit_counter  = 0xFFFF;
    pit_latched  = false;
    pit_read_lo  = true;
    kprintf("platform: PIT/CMOS/PS2 stubs initialised\n");
}

bool pit_handle_io(uint16_t port, bool is_in, uint32_t size, uint64_t *val) {
    UNUSED(size);

    if (port < 0x40 || port > 0x43)
        return false;

    if (is_in) {
        if (port == 0x40) {
            if (!pit_latched) {
                pit_counter -= 100;
            }
            if (pit_read_lo) {
                *val = pit_counter & 0xFF;
                pit_read_lo = false;
            } else {
                *val = (pit_counter >> 8) & 0xFF;
                pit_read_lo = true;
                pit_latched = false;
            }
        } else {
            *val = 0;
        }
    } else {
        if (port == 0x43) {
            uint8_t cmd = static_cast<uint8_t>(*val);
            if ((cmd & 0x30) == 0) {
                pit_latched = true;
                pit_read_lo = true;
            }
        }
    }

    return true;
}

// ── CMOS / RTC ───────────────────────────────────────────────────────

static uint8_t cmos_index;

bool cmos_handle_io(uint16_t port, bool is_in, uint32_t size, uint64_t *val) {
    UNUSED(size);

    if (port != 0x70 && port != 0x71)
        return false;

    if (port == 0x70) {
        if (!is_in) {
            cmos_index = static_cast<uint8_t>(*val & 0x7F);
        } else {
            *val = cmos_index;
        }
        return true;
    }

    if (is_in) {
        switch (cmos_index) {
        case 0x00: *val = 0x00; break; // seconds
        case 0x02: *val = 0x00; break; // minutes
        case 0x04: *val = 0x12; break; // hours (12:00)
        case 0x06: *val = 0x01; break; // day of week
        case 0x07: *val = 0x01; break; // day of month
        case 0x08: *val = 0x01; break; // month
        case 0x09: *val = 0x26; break; // year (2026 - 2000 = 26 BCD)
        case 0x0A: *val = 0x26; break; // Status Register A (UIP=0, divider)
        case 0x0B: *val = 0x02; break; // Status Register B (24h, BCD)
        case 0x0C: *val = 0x00; break; // Status Register C (no IRQ pending)
        case 0x0D: *val = 0x80; break; // Status Register D (valid RAM)
        case 0x0F: *val = 0x00; break; // shutdown status
        default:   *val = 0x00; break;
        }
    }

    return true;
}

// ── PS/2 controller ──────────────────────────────────────────────────

bool ps2_handle_io(uint16_t port, bool is_in, uint32_t size, uint64_t *val) {
    UNUSED(size);

    if (port == 0x60) {
        if (is_in) *val = 0;
        return true;
    }

    if (port == 0x64) {
        if (is_in) *val = 0x00;
        return true;
    }

    return false;
}

#endif
