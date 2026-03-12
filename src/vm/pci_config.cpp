#include "x86/pci.h"
#include "string.h"
#include "console.h"

#ifdef __x86_64__

static constexpr uint16_t PCI_ADDR_PORT = 0xCF8;
static constexpr uint16_t PCI_DATA_PORT = 0xCFC;

static constexpr uint32_t PCI_CONFIG_SIZE = 256;

static uint8_t host_bridge_cfg[PCI_CONFIG_SIZE];
static uint8_t ide_ctrl_cfg[PCI_CONFIG_SIZE];

static uint32_t pci_addr_reg;

static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

void pci_init() {
    pci_addr_reg = 0;

    memset(host_bridge_cfg, 0, PCI_CONFIG_SIZE);
    write_le16(host_bridge_cfg + 0x00, 0x8086);
    write_le16(host_bridge_cfg + 0x02, 0x1237);
    host_bridge_cfg[0x0B] = 0x06;
    host_bridge_cfg[0x0E] = 0x00;

    memset(ide_ctrl_cfg, 0, PCI_CONFIG_SIZE);
    write_le16(ide_ctrl_cfg + 0x00, 0x8086);
    write_le16(ide_ctrl_cfg + 0x02, 0x7010);
    ide_ctrl_cfg[0x09] = 0x8A;
    ide_ctrl_cfg[0x0A] = 0x01;
    ide_ctrl_cfg[0x0B] = 0x01;
    ide_ctrl_cfg[0x0E] = 0x00;
    ide_ctrl_cfg[0x3C] = 14;

    kprintf("pci: virtual bus initialised (host bridge + IDE controller)\n");
}

static const uint8_t *find_device(uint8_t bus, uint8_t dev, uint8_t func) {
    if (bus != 0)
        return nullptr;

    if (dev == 0 && func == 0)
        return host_bridge_cfg;
    if (dev == 1 && func == 1)
        return ide_ctrl_cfg;

    return nullptr;
}

bool pci_handle_io(uint16_t port, bool is_in, uint32_t size, uint64_t *val) {
    if (port == PCI_ADDR_PORT) {
        if (is_in) {
            *val = pci_addr_reg;
        } else {
            pci_addr_reg = static_cast<uint32_t>(*val);
        }
        return true;
    }

    if (port >= PCI_DATA_PORT && port <= PCI_DATA_PORT + 3) {
        if (!(pci_addr_reg & 0x80000000)) {
            if (is_in) *val = 0xFFFFFFFF;
            return true;
        }

        uint8_t bus  = static_cast<uint8_t>((pci_addr_reg >> 16) & 0xFF);
        uint8_t dev  = static_cast<uint8_t>((pci_addr_reg >> 11) & 0x1F);
        uint8_t func = static_cast<uint8_t>((pci_addr_reg >> 8)  & 0x07);
        uint8_t reg  = static_cast<uint8_t>((pci_addr_reg & 0xFC)
                                            + (port - PCI_DATA_PORT));

        const uint8_t *cfg = find_device(bus, dev, func);
        if (!cfg) {
            if (is_in) *val = 0xFFFFFFFF;
            return true;
        }

        if (is_in) {
            if (static_cast<uint32_t>(reg) + 4 <= PCI_CONFIG_SIZE)
                *val = read_le32(cfg + reg);
            else
                *val = 0xFFFFFFFF;

            if (size == 1) *val = (*val >> ((port & 3) * 8)) & 0xFF;
            else if (size == 2) *val = (*val >> ((port & 2) * 8)) & 0xFFFF;
        } else {
            UNUSED(val);
        }

        return true;
    }

    return false;
}

#endif
