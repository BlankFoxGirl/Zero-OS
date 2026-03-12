#include "x86/ide.h"
#include "string.h"
#include "console.h"

#ifdef __x86_64__

static constexpr uint16_t IDE_DATA       = 0x1F0;
static constexpr uint16_t IDE_ERROR      = 0x1F1;
static constexpr uint16_t IDE_SECT_COUNT = 0x1F2;
static constexpr uint16_t IDE_LBA_LO    = 0x1F3;
static constexpr uint16_t IDE_LBA_MID   = 0x1F4;
static constexpr uint16_t IDE_LBA_HI    = 0x1F5;
static constexpr uint16_t IDE_DRIVE_HEAD = 0x1F6;
static constexpr uint16_t IDE_STATUS     = 0x1F7;
static constexpr uint16_t IDE_COMMAND    = 0x1F7;
static constexpr uint16_t IDE_ALT_STATUS = 0x3F6;
static constexpr uint16_t IDE_DEV_CTRL   = 0x3F6;

static constexpr uint8_t STATUS_BSY   = 0x80;
static constexpr uint8_t STATUS_DRDY  = 0x40;
static constexpr uint8_t STATUS_DRQ   = 0x08;
static constexpr uint8_t STATUS_ERR   = 0x01;

static constexpr uint8_t CMD_READ_SECTORS     = 0x20;
static constexpr uint8_t CMD_READ_SECTORS_EXT = 0x24;
static constexpr uint8_t CMD_IDENTIFY         = 0xEC;

static constexpr uint32_t SECTOR_SIZE = 512;

struct IdeState {
    const uint8_t *disk;
    uint64_t       disk_sectors;

    uint8_t  status;
    uint8_t  error;
    uint8_t  drive_head;

    uint8_t  sector_count;
    uint8_t  lba_lo;
    uint8_t  lba_mid;
    uint8_t  lba_hi;

    uint8_t  hob_sector_count;
    uint8_t  hob_lba_lo;
    uint8_t  hob_lba_mid;
    uint8_t  hob_lba_hi;
    bool     hob_written;

    uint16_t data_buf[256];
    uint32_t data_pos;
    uint32_t data_len;
    uint32_t sectors_remaining;
    uint64_t current_lba;

    bool     irq_pending;
};

static IdeState ide;

static void fill_identify(uint16_t *buf) {
    memset(buf, 0, 512);

    buf[0]  = 0x0040;
    buf[1]  = static_cast<uint16_t>(ide.disk_sectors > 16383 ? 16383
                                    : ide.disk_sectors);
    buf[3]  = 16;
    buf[6]  = 63;
    buf[47] = 0x8010;
    buf[49] = (1 << 9);
    buf[53] = 0x0006;
    buf[60] = static_cast<uint16_t>(ide.disk_sectors & 0xFFFF);
    buf[61] = static_cast<uint16_t>((ide.disk_sectors >> 16) & 0xFFFF);
    buf[80] = (1 << 6);
    buf[83] = (1 << 10);
    buf[86] = (1 << 10);
    buf[100] = static_cast<uint16_t>(ide.disk_sectors & 0xFFFF);
    buf[101] = static_cast<uint16_t>((ide.disk_sectors >> 16) & 0xFFFF);
    buf[102] = static_cast<uint16_t>((ide.disk_sectors >> 32) & 0xFFFF);
    buf[103] = static_cast<uint16_t>((ide.disk_sectors >> 48) & 0xFFFF);

    const char *serial = "ZEROOS00000000001234";
    for (int i = 0; i < 10; i++) {
        buf[10 + i] = static_cast<uint16_t>(
            (static_cast<uint8_t>(serial[i * 2]) << 8) |
             static_cast<uint8_t>(serial[i * 2 + 1]));
    }

    const char *fw = "ZOS 1.0 ";
    for (int i = 0; i < 4; i++) {
        buf[23 + i] = static_cast<uint16_t>(
            (static_cast<uint8_t>(fw[i * 2]) << 8) |
             static_cast<uint8_t>(fw[i * 2 + 1]));
    }

    const char *model = "ZeroOS Virtual Disk                     ";
    for (int i = 0; i < 20; i++) {
        buf[27 + i] = static_cast<uint16_t>(
            (static_cast<uint8_t>(model[i * 2]) << 8) |
             static_cast<uint8_t>(model[i * 2 + 1]));
    }
}

static void prepare_sector_read() {
    if (ide.current_lba >= ide.disk_sectors) {
        ide.status = STATUS_DRDY | STATUS_ERR;
        ide.error  = 0x04;
        return;
    }

    uint64_t offset = ide.current_lba * SECTOR_SIZE;
    memcpy(ide.data_buf, ide.disk + offset, SECTOR_SIZE);
    ide.data_pos = 0;
    ide.data_len = 256;
    ide.status   = STATUS_DRDY | STATUS_DRQ;
    ide.irq_pending = true;
}

static void handle_command(uint8_t cmd) {
    switch (cmd) {
    case CMD_IDENTIFY:
        fill_identify(ide.data_buf);
        ide.data_pos = 0;
        ide.data_len = 256;
        ide.status   = STATUS_DRDY | STATUS_DRQ;
        ide.error    = 0;
        ide.irq_pending = true;
        break;

    case CMD_READ_SECTORS: {
        uint32_t lba = static_cast<uint32_t>(ide.lba_lo)
                     | (static_cast<uint32_t>(ide.lba_mid) << 8)
                     | (static_cast<uint32_t>(ide.lba_hi)  << 16)
                     | (static_cast<uint32_t>(ide.drive_head & 0x0F) << 24);
        uint32_t count = ide.sector_count ? ide.sector_count : 256;
        ide.current_lba       = lba;
        ide.sectors_remaining = count;
        ide.error = 0;
        prepare_sector_read();
        break;
    }

    case CMD_READ_SECTORS_EXT: {
        uint64_t lba = static_cast<uint64_t>(ide.lba_lo)
                     | (static_cast<uint64_t>(ide.lba_mid) << 8)
                     | (static_cast<uint64_t>(ide.lba_hi)  << 16)
                     | (static_cast<uint64_t>(ide.hob_lba_lo)  << 24)
                     | (static_cast<uint64_t>(ide.hob_lba_mid) << 32)
                     | (static_cast<uint64_t>(ide.hob_lba_hi)  << 40);
        uint32_t count = static_cast<uint32_t>(ide.sector_count)
                       | (static_cast<uint32_t>(ide.hob_sector_count) << 8);
        if (count == 0) count = 65536;
        ide.current_lba       = lba;
        ide.sectors_remaining = count;
        ide.error = 0;
        prepare_sector_read();
        break;
    }

    default:
        kprintf("ide: unknown command 0x%02x\n", cmd);
        ide.status = STATUS_DRDY | STATUS_ERR;
        ide.error  = 0x04;
        break;
    }
}

void ide_init(uint64_t disk_hpa, uint64_t disk_size) {
    memset(&ide, 0, sizeof(ide));
    ide.disk         = reinterpret_cast<const uint8_t *>(disk_hpa);
    ide.disk_sectors = disk_size / SECTOR_SIZE;
    ide.status       = STATUS_DRDY;
    ide.drive_head   = 0xA0;
    kprintf("ide: virtual disk %llu MiB (%llu sectors)\n",
            (unsigned long long)(disk_size / (1024 * 1024)),
            (unsigned long long)ide.disk_sectors);
}

bool ide_handle_io(uint16_t port, bool is_in, uint32_t size, uint64_t *val) {
    if (!((port >= 0x1F0 && port <= 0x1F7) || port == 0x3F6))
        return false;

    if (is_in) {
        switch (port) {
        case IDE_DATA:
            if (ide.data_pos < ide.data_len) {
                *val = ide.data_buf[ide.data_pos++];
                if (ide.data_pos >= ide.data_len) {
                    ide.sectors_remaining--;
                    if (ide.sectors_remaining > 0) {
                        ide.current_lba++;
                        prepare_sector_read();
                    } else {
                        ide.status = STATUS_DRDY;
                    }
                }
            } else {
                *val = 0;
            }
            break;
        case IDE_ERROR:
            *val = ide.error;
            break;
        case IDE_SECT_COUNT:
            *val = ide.sector_count;
            break;
        case IDE_LBA_LO:
            *val = ide.lba_lo;
            break;
        case IDE_LBA_MID:
            *val = ide.lba_mid;
            break;
        case IDE_LBA_HI:
            *val = ide.lba_hi;
            break;
        case IDE_DRIVE_HEAD:
            *val = ide.drive_head;
            break;
        case IDE_STATUS:
            ide.irq_pending = false;
            *val = ide.status;
            break;
        case IDE_ALT_STATUS:
            *val = ide.status;
            break;
        default:
            *val = 0;
            break;
        }
    } else {
        uint8_t b = static_cast<uint8_t>(*val & 0xFF);
        switch (port) {
        case IDE_DATA:
            break;
        case IDE_ERROR:
            break;
        case IDE_SECT_COUNT:
            ide.hob_sector_count = ide.sector_count;
            ide.sector_count = b;
            break;
        case IDE_LBA_LO:
            ide.hob_lba_lo = ide.lba_lo;
            ide.lba_lo = b;
            break;
        case IDE_LBA_MID:
            ide.hob_lba_mid = ide.lba_mid;
            ide.lba_mid = b;
            break;
        case IDE_LBA_HI:
            ide.hob_lba_hi = ide.lba_hi;
            ide.lba_hi = b;
            break;
        case IDE_DRIVE_HEAD:
            ide.drive_head = b;
            break;
        case IDE_COMMAND:
            handle_command(b);
            break;
        case IDE_DEV_CTRL:
            break;
        default:
            break;
        }
    }

    UNUSED(size);
    return true;
}

#endif
