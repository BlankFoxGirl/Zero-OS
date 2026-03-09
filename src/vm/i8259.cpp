#include "x86/pic.h"
#include "string.h"
#include "console.h"

#ifdef __x86_64__

struct PicChip {
    uint8_t  irr;
    uint8_t  isr;
    uint8_t  imr;
    uint8_t  vector_base;
    uint8_t  icw_step;
    bool     icw4_needed;
    bool     read_isr;
};

static PicChip master;
static PicChip slave;

void pic_init() {
    memset(&master, 0, sizeof(master));
    memset(&slave,  0, sizeof(slave));

    master.imr = 0xFF;
    slave.imr  = 0xFF;
    master.vector_base = 0x08;
    slave.vector_base  = 0x70;

    kprintf("pic: dual i8259 initialised\n");
}

static void pic_write(PicChip *chip, uint16_t port_offset, uint8_t val) {
    if (port_offset == 0) {
        if (val & 0x10) {
            chip->icw_step   = 1;
            chip->icw4_needed = (val & 0x01) != 0;
            chip->imr = 0;
            chip->isr = 0;
            chip->irr = 0;
            return;
        }

        if (val & 0x20) {
            for (int i = 0; i < 8; i++) {
                if (chip->isr & (1 << i)) {
                    chip->isr &= ~(1 << i);
                    break;
                }
            }
            return;
        }

        if (val == 0x0B) { chip->read_isr = true; return; }
        if (val == 0x0A) { chip->read_isr = false; return; }

        if (val & 0x60) {
            uint8_t level = val & 0x07;
            chip->isr &= ~(1 << level);
        }
    } else {
        switch (chip->icw_step) {
        case 1:
            chip->vector_base = val & 0xF8;
            chip->icw_step = 2;
            break;
        case 2:
            chip->icw_step = chip->icw4_needed ? 3 : 0;
            break;
        case 3:
            chip->icw_step = 0;
            break;
        default:
            chip->imr = val;
            break;
        }
    }
}

static uint8_t pic_read(PicChip *chip, uint16_t port_offset) {
    if (port_offset == 0)
        return chip->read_isr ? chip->isr : chip->irr;
    return chip->imr;
}

void pic_raise_irq(uint8_t irq) {
    if (irq < 8) {
        master.irr |= (1 << irq);
    } else if (irq < 16) {
        slave.irr |= (1 << (irq - 8));
        master.irr |= (1 << 2);
    }
}

bool pic_pending(uint8_t *out_vector) {
    uint8_t unmasked = master.irr & ~master.imr & ~master.isr;
    if (!unmasked)
        return false;

    for (int i = 0; i < 8; i++) {
        if (!(unmasked & (1 << i)))
            continue;

        if (i == 2) {
            uint8_t s_unmasked = slave.irr & ~slave.imr & ~slave.isr;
            if (!s_unmasked) continue;
            for (int j = 0; j < 8; j++) {
                if (s_unmasked & (1 << j)) {
                    slave.irr  &= ~(1 << j);
                    slave.isr  |=  (1 << j);
                    master.irr &= ~(1 << 2);
                    master.isr |=  (1 << 2);
                    *out_vector = slave.vector_base + j;
                    return true;
                }
            }
            continue;
        }

        master.irr &= ~(1 << i);
        master.isr |=  (1 << i);
        *out_vector = master.vector_base + i;
        return true;
    }

    return false;
}

bool pic_handle_io(uint16_t port, bool is_in, uint32_t size, uint64_t *val) {
    UNUSED(size);

    if (port == 0x20 || port == 0x21) {
        if (is_in)
            *val = pic_read(&master, port - 0x20);
        else
            pic_write(&master, port - 0x20, static_cast<uint8_t>(*val));
        return true;
    }

    if (port == 0xA0 || port == 0xA1) {
        if (is_in)
            *val = pic_read(&slave, port - 0xA0);
        else
            pic_write(&slave, port - 0xA0, static_cast<uint8_t>(*val));
        return true;
    }

    return false;
}

#endif
